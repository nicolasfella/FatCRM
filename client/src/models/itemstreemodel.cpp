/*
  This file is part of FatCRM, a desktop application for SugarCRM written by KDAB.

  Copyright (C) 2015-2021 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Authors: David Faure <david.faure@kdab.com>
           Michel Boyer de la Giroday <michel.giroday@kdab.com>
           Kevin Krammer <kevin.krammer@kdab.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "itemstreemodel.h"
#include "referenceddata.h"
#include "clientsettings.h"
#include "linkeditemsrepository.h"
#include "collectionmanager.h"
#include "fatcrm_client_debug.h"

#include "kdcrmdata/sugaraccount.h"
#include "kdcrmdata/sugarcampaign.h"
#include "kdcrmdata/sugarlead.h"
#include "kdcrmdata/sugaropportunity.h"
#include "kdcrmdata/kdcrmutils.h"
#include "kdcrmdata/kdcrmfields.h"

#include <KContacts/Addressee>
#include <KContacts/PhoneNumber>

#include <AkonadiCore/ChangeRecorder>
#include <AkonadiCore/ItemModifyJob>
#include <AkonadiCore/EntityDisplayAttribute>
#include <akonadi_version.h>

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <KColorScheme>
#include <KColorUtils>
#include <KIconLoader>
#include <KLocalizedString>
#include <QMetaEnum>
#include <QElapsedTimer>
#include <QHash>
#include <QScopedValueRollback>
#include <sugarcontactwrapper.h>

using namespace Akonadi;

namespace {

// TODO: Use std::clamp once we require C++17
template<class T>
const T& clamp( const T& v, const T& lo, const T& hi )
{
    Q_ASSERT(!(hi < lo));
    return (v < lo) ? lo : (hi < v) ? hi : v;
}

QString countryNameTo2DigitCode(const QString& countryName)
{
    auto create = []() -> QHash<QString, QString>
    {
        // build the country name to 2-digit-code map
        QFile file(":/flags/countrylist.json");
        bool success = file.open(QIODevice::ReadOnly);
        Q_ASSERT(success);
        if (!success) {
            qWarning() << "Failed to load countrylist.json";
            return {};
        }

        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        auto countryListArray = doc.array();

        QHash<QString, QString> countryNameTo2DigitCodeMap;
        for (auto it = countryListArray.begin(), end = countryListArray.end(); it != end; ++it) {
            const QString &code = it->toObject().value("Code").toString();
            const QString &name = it->toObject().value("Name").toString();
            countryNameTo2DigitCodeMap.insert(name, code.toLower());
        }

        // add some more common country name mappings
        countryNameTo2DigitCodeMap.unite({
            {"USA", "us"},
            {"UK", "gb"},
            {"South Korea", "kr"},
            {"The Netherlands", "nl"}
        });

        return countryNameTo2DigitCodeMap;
    };

    static const auto countryNameTo2DigitCodeMap = create();

    return countryNameTo2DigitCodeMap.value(countryName);
}

}

class ItemsTreeModel::Private
{
public:
    struct BackgroundColors
    {
        QColor lastModifiedDateBackground;
        QColor nextStepDateBackground;
        QColor closeDateBackground;

        bool containsValidColors() const
        {
            return lastModifiedDateBackground.isValid() || nextStepDateBackground.isValid() || closeDateBackground.isValid();
        }
    };

    Private()
        : mColumns(),
          mIconSize(KIconLoader::global()->currentSize(KIconLoader::Small))
    {
    }

    ItemsTreeModel::ColumnTypes mColumns;
    QHash<Item::Id, BackgroundColors> mIdToBackgroundColorsMap;
    bool mCurrentlyUpdatingBackgrounds = false;
    const int mIconSize;
};

ItemsTreeModel::ItemsTreeModel(DetailsType type, ChangeRecorder *monitor, QObject *parent)
    : EntityTreeModel(monitor, parent), d(new Private), mType(type)
{
    d->mColumns = columnTypes(mType);

    if (mType == DetailsType::Opportunity) {
        // Update accountName and country columns once all accounts are loaded
        connect(AccountRepository::instance(), &AccountRepository::initialLoadingDone,
                this, &ItemsTreeModel::slotAccountsLoaded);

        // and update it again later in case of single changes (by the user or when updating from server)
        connect(AccountRepository::instance(), &AccountRepository::accountModified,
                this, &ItemsTreeModel::slotAccountModified);

        // React to account removals
        connect(AccountRepository::instance(), &AccountRepository::accountRemoved,
                this, &ItemsTreeModel::slotAccountRemoved);
    }

    connect(this, &EntityTreeModel::rowsInserted, [this](const QModelIndex&, int first, int last) {
        updateBackgrounds(first, last);
    });
    connect(this, &EntityTreeModel::dataChanged, [this](const QModelIndex& topLeft, const QModelIndex& bottomRight) {
        updateBackgrounds(topLeft.row(), bottomRight.row());
    });
    updateBackgrounds();
}

ItemsTreeModel::~ItemsTreeModel()
{
    delete d;
}

/**
 * Returns the columns that the model currently shows.
 */
ItemsTreeModel::ColumnTypes ItemsTreeModel::columnTypes() const
{
    return d->mColumns;
}

void ItemsTreeModel::updateBackgrounds()
{
    if (rowCount() == 0)
        return;

    updateBackgrounds(0, rowCount() - 1);
}

void ItemsTreeModel::updateBackgrounds(int first, int last)
{
    Q_ASSERT(first >= 0 && last >= 0 && last < rowCount());

    if (d->mCurrentlyUpdatingBackgrounds)
        return;

    QScopedValueRollback<bool> reentrancyBlocker(d->mCurrentlyUpdatingBackgrounds, true);

    const int lastModifiedDateColumn = d->mColumns.indexOf(LastModifiedDate);
    const int nextStepDateColumn = d->mColumns.indexOf(NextStepDate);
    const int closeDateColumn = d->mColumns.indexOf(CloseDate);
    if (lastModifiedDateColumn == -1 && nextStepDateColumn == -1 && closeDateColumn == -1)
        return; // no backgrounds to show, nothing to do for this model

    QElapsedTimer timer;
    timer.start();

    const KColorScheme colors(QPalette::Active);
    // TODO: Take alternating row colors into account? How?
    const QColor background = colors.background().color();

    const auto currentDate = QDate::currentDate();

    for (int row = first; row <= last; ++row) {
        const Item item = index(row, 0).data(ItemRole).value<Item>();
        Q_ASSERT(item.isValid());

        Private::BackgroundColors bgColors;

        if (lastModifiedDateColumn != -1) {
            // LastModifiedDate => Indicate whether opps are still 'warm', cf. FATCRM-109
            const qint64 daysAfterOppIsCold = 90;
            const auto date = entityData(item, lastModifiedDateColumn, Qt::EditRole).toDateTime().date();
            const qint64 daysSinceLastModificationDate = ::clamp(date.daysTo(currentDate), 0ll, daysAfterOppIsCold);
            if (daysSinceLastModificationDate != daysAfterOppIsCold) {
                const qreal amount = (float)qAbs(daysSinceLastModificationDate - daysAfterOppIsCold) / daysAfterOppIsCold;
                bgColors.lastModifiedDateBackground = KColorUtils::mix(background, colors.background(KColorScheme::PositiveBackground).color(), amount);
            }
        }

        if (nextStepDateColumn != -1) {
            // NextStepDate => Indicate whether next steps are due
            const qint64 daysPastNextStepDateWhenHeavilyDue = 90;
            const auto date = entityData(item, nextStepDateColumn, Qt::EditRole).toDateTime().date();
            // means: after this time we consider the next step heavily due
            const qint64 daysPastNextStepDate = ::clamp(date.daysTo(currentDate), 0ll, daysPastNextStepDateWhenHeavilyDue);
            if (daysPastNextStepDate != 0) {
                // use a min amount of 0.3f since we should definitely notice the indicator when we're past the due date
                const qreal amount = qMax(0.3f, (float)daysPastNextStepDate / daysPastNextStepDateWhenHeavilyDue);
                bgColors.nextStepDateBackground = KColorUtils::mix(background, colors.background(KColorScheme::NegativeBackground).color(), amount);
            }
        }

        if (closeDateColumn != -1) {
            // CloseDate => Indicate whether close date is due
            const qint64 daysPastCloseDateWhenHeavilyDue = 90;
            const auto date = entityData(item, closeDateColumn, Qt::EditRole).toDate();
            // means: after this time we consider the close step heavily due
            const qint64 secsPastNextStepDate = ::clamp(date.daysTo(currentDate), 0ll, daysPastCloseDateWhenHeavilyDue);
            if (secsPastNextStepDate != 0) {
                // use a min amount of 0.3f since we should definitely notice the indicator when we're past the due date
                const qreal amount = qMax(0.3f, (float)secsPastNextStepDate / daysPastCloseDateWhenHeavilyDue);
                bgColors.closeDateBackground = KColorUtils::mix(background, colors.background(KColorScheme::NegativeBackground).color(), amount);
            }
        }

        if (bgColors.containsValidColors()) {
            d->mIdToBackgroundColorsMap[item.id()] = bgColors;
        } else {
            d->mIdToBackgroundColorsMap.remove(item.id());
        }
    }

    if (lastModifiedDateColumn != -1) {
        emit dataChanged(index(first, lastModifiedDateColumn), index(last, lastModifiedDateColumn));
    }
    if (nextStepDateColumn != -1) {
        emit dataChanged(index(first, nextStepDateColumn), index(last, nextStepDateColumn));
    }
    if (closeDateColumn != -1) {
        emit dataChanged(index(first, closeDateColumn), index(last, closeDateColumn));
    }

    qCDebug(FATCRM_CLIENT_LOG) << "Done updating backgrounds for" << (last-first+1) << "items in" << timer.elapsed() << "ms";
}

/**
 * Reimp: Returns the data displayed by the model
 */
QVariant ItemsTreeModel::entityData(const Item &item, int column, int role) const
{
    switch (role) {
    case Qt::DecorationRole:
        switch (columnTypes().at(column)) {
        case Country: {
            const QString country = entityData(item, column, Qt::DisplayRole).toString();
            const QString countryCode = countryNameTo2DigitCode(country);
            if (countryCode.isEmpty())
                return {};

            return QIcon(QStringLiteral(":/flags/%1.svg").arg(countryCode));
        }
        default:
            return {};
        }
        break;

    case Qt::DisplayRole:
    case Qt::EditRole:
        switch (mType) {
        case DetailsType::Account:
            return accountData(item, column, role);
        case DetailsType::Campaign:
            return campaignData(item, column, role);
        case DetailsType::Contact:
            return contactData(item, column, role);
        case DetailsType::Lead:
            return leadData(item, column, role);
        case DetailsType::Opportunity:
            return opportunityData(item, column, role);
        }
        break;

    case Qt::BackgroundRole:
        switch (columnTypes().at(column)) {
        case NextStepDate: {
            auto color = d->mIdToBackgroundColorsMap.value(item.id()).nextStepDateBackground;
            return color.isValid() ? color : QVariant();
        }
        case LastModifiedDate: {
            auto color = d->mIdToBackgroundColorsMap.value(item.id()).lastModifiedDateBackground;
            return color.isValid() ? color : QVariant();
        }
        case CloseDate: {
            auto color = d->mIdToBackgroundColorsMap.value(item.id()).closeDateBackground;
            return color.isValid() ? color : QVariant();
        }
        default:
            return {};
        }
        break;

    case Qt::ToolTipRole:
        if (ClientSettings::self()->showToolTips()) {
            if (mType == DetailsType::Account) {
                return accountToolTip(item);
            } else if (mType == DetailsType::Opportunity) {
                return opportunityToolTip(item);
            }
        }
        return QVariant();

    case Qt::FontRole:
        if (mType == DetailsType::Opportunity) {
            return opportunityData(item, column, role);
        } else if (mType == DetailsType::Contact) {
            return contactData(item, column, role);
        }
        break;

    default:
        break;
    }

    return EntityTreeModel::entityData(item, column, role);
}

/**
 * Reimp
 */
QVariant ItemsTreeModel::entityData(const Collection &collection, int column, int role) const
{
    if (role == Qt::DisplayRole) {
        switch (column) {
        case 0:
            return EntityTreeModel::entityData(collection, column, role);
        default:
            return QString(); // pass model test
        }
    }

    return EntityTreeModel::entityData(collection, column, role);
}

/**
 * Reimp
 */
int ItemsTreeModel::entityColumnCount(HeaderGroup headerGroup) const
{
    if (headerGroup == EntityTreeModel::CollectionTreeHeaders) {
        return 1;
    } else if (headerGroup == EntityTreeModel::ItemListHeaders) {
        return d->mColumns.count();
    } else {
        return EntityTreeModel::entityColumnCount(headerGroup);
    }
}

QVariant ItemsTreeModel::data(const QModelIndex &index, int role) const
{
    if (role == ColumnTypeRole) {
        return d->mColumns.value(index.column());
    }

    return EntityTreeModel::data(index, role);
}

void ItemsTreeModel::setLinkedItemsRepository(LinkedItemsRepository *repo)
{
    mLinkedItemsRepository = repo;
}

void ItemsTreeModel::setCollectionManager(CollectionManager *collectionManager)
{
    mCollectionManager = collectionManager;
}

QString ItemsTreeModel::countryForContact(const KContacts::Addressee &addressee)
{
    // Get the country from the contact, if it has an address.
    const QString cc = addressee.address(KContacts::Address::Work | KContacts::Address::Pref).country();
    if (!cc.isEmpty())
        return cc;
    // Otherwise get the country via the account
    const QString country = AccountRepository::instance()->accountById(addressee.organization()).countryForGui();
    return country;
}

void ItemsTreeModel::slotAccountModified(const QString &accountId, const QVector<AccountRepository::Field> &changedFields)
{
    if (mType == DetailsType::Opportunity) {
        // Find which opps use that account
        const int rows = rowCount();
        if (rows == 0)
            return;
        QVector<int> columns;
        if (changedFields.contains(AccountRepository::Country)) {
            columns.append(d->mColumns.indexOf(Country));
        }
        if (changedFields.contains(AccountRepository::Name)) {
            columns.append(d->mColumns.indexOf(OpportunityAccountName));
        }
        if (columns.isEmpty())
            return;
        const int firstColumn = *std::min_element(columns.constBegin(), columns.constEnd());
        const int lastColumn = *std::max_element(columns.constBegin(), columns.constEnd());
        for (int i = 0; i < rows; ++i) {
            const QModelIndex idx = index(i, 0);
            const Akonadi::Item item = data(idx, EntityTreeModel::ItemRole).value<Item>();
            const SugarOpportunity opportunity = item.payload<SugarOpportunity>();
            if (opportunity.accountId() == accountId) {
                emit dataChanged(idx.sibling(i, firstColumn), idx.sibling(i, lastColumn), {Qt::DisplayRole});
            }
        }
    }
}

void ItemsTreeModel::slotAccountRemoved(const QString &accountId)
{
    if (mType == DetailsType::Opportunity) {
        // The opps that were using this account need to be synced explicitly, this might be the result of an account merge
        // and SugarCRM doesn't mark the opps as modified when this happens...
        const int rows = rowCount();
        for (int row = 0; row < rows; ++row) {
            const Akonadi::Item item = index(row, 0).data(Akonadi::EntityTreeModel::ItemRole).value<Akonadi::Item>();
            const SugarOpportunity opp = item.payload<SugarOpportunity>();
            if (opp.accountId() == accountId) {
                // force-sync this opp (this "strange" code is unittested in forceRefreshShouldCallResource)
                qCDebug(FATCRM_CLIENT_LOG) << "opp" << opp.name() << "is using deleted account" << accountId;

                // Clear the payload, to force a refetch from the resource
                Item fakeItem(item.id());
                fakeItem.clearPayload();
                // Workaround akonadi bug which was fixed in akonadi 5.7.3 (commit a2a85090c)
#if AKONADI_VERSION < QT_VERSION_CHECK(5, 7, 3) || AKONADI_VERSION == QT_VERSION_CHECK(5, 7, 40)
                fakeItem.addAttribute(new Akonadi::EntityDisplayAttribute());
                fakeItem.removeAttribute<Akonadi::EntityDisplayAttribute>();
#endif
                auto *modifyJob = new Akonadi::ItemModifyJob(fakeItem, this);
                connect(modifyJob, &Akonadi::ItemModifyJob::result, this, []() {
                    qCDebug(FATCRM_CLIENT_LOG) << "ItemModifyJob is done";
                });
            }
        }
    }
}

// Called when the accounts have just been loaded
// Normally we have no opps yet, but it can happen if akonadi syncs the folders in a different order than we expected
// (i.e. due to queued jobs in the resource)
void ItemsTreeModel::slotAccountsLoaded()
{
    const int rows = rowCount();
    if (rows > 0) {
        const int columnCountry = d->mColumns.indexOf(Country);
        const int columnAccountName = d->mColumns.indexOf(OpportunityAccountName);
        const int firstColumn = qMin(columnCountry, columnAccountName);
        const int lastColumn = qMax(columnCountry, columnAccountName);
        qCDebug(FATCRM_CLIENT_LOG) << "emit dataChanged" << 0 << firstColumn << rows-1 << lastColumn;
        emit dataChanged(index(0, firstColumn), index(rows - 1, lastColumn));
    }
}

/**
 * Reimp: Return the Header data to display
 */
QVariant ItemsTreeModel::entityHeaderData(int section, Qt::Orientation orientation, int role, HeaderGroup headerGroup) const
{
    if (role == Qt::DisplayRole) {
        if (orientation == Qt::Horizontal) {
            if (headerGroup == EntityTreeModel::CollectionTreeHeaders) {

                if (section >= 1) {
                    return QVariant();
                }

                switch (section) {
                case 0:
                    return i18nc("@title:Sugar items overview", "Items");
                    break;
                }
            } else { // e.g EntityTreeModel::ItemListHeaders
                if (section < 0 || section >= d->mColumns.count()) {
                    return QVariant();
                }
                return columnTitle(d->mColumns.at(section));
            }
        }
    }

    if (role == Qt::ToolTipRole) {
        return columnToolTip(d->mColumns.at(section));
    }

    return EntityTreeModel::entityHeaderData(section, orientation, role, headerGroup);
}

/**
 * Return the data. SugarAccount type
 */
QVariant ItemsTreeModel::accountData(const Item &item, int column, int role) const
{
    if (!item.hasPayload<SugarAccount>()) {
        // Pass modeltest
        if (role == Qt::DisplayRole) {
            return item.remoteId();
        }

        return QVariant();
    }
    const SugarAccount account = item.payload<SugarAccount>();

    if ((role == Qt::DisplayRole) || (role == Qt::EditRole)) {
        switch (columnTypes().at(column)) {
        case Name:
            return account.name();
        case City:
            return account.shippingAddressCity().isEmpty() ? account.billingAddressCity() : account.shippingAddressCity();
        case Country:
            return account.shippingAddressCountry().isEmpty() ? account.billingAddressCountry() : account.shippingAddressCountry();
        case Street:
            return account.shippingAddressStreet().isEmpty() ? account.billingAddressStreet() : account.shippingAddressStreet();
        case Phone:
            return account.phoneOffice();
        case Email:
            return account.email1();
        case CreatedBy:
            return account.createdByName();
        case PostalCode:
            return account.postalCodeForGui();
        case NumberOfOpportunities:
            return mLinkedItemsRepository->opportunitiesForAccount(account.id()).count();
        case NumberOfContacts:
            return mLinkedItemsRepository->contactsForAccount(account.id()).count();
        case NumberOfDocumentsNotesEmails:
            // The goal is to find those with 0 of each (for GDPR cleanup purposes)
            // so I'm not doing 3 different columns, for now.
            return mLinkedItemsRepository->documentsForAccount(account.id()).count() +
                    mLinkedItemsRepository->notesForAccount(account.id()).count() +
                    mLinkedItemsRepository->emailsForAccount(account.id()).count();
        default:
            return QVariant();
        }
    }

    return QVariant();
}

/**
 * Return the data. SugarCampaign type
 */
QVariant ItemsTreeModel::campaignData(const Item &item, int column, int role) const
{
    if (!item.hasPayload<SugarCampaign>()) {

        // Pass modeltest
        if (role == Qt::DisplayRole) {
            return item.remoteId();
        }

        return QVariant();
    }

    const SugarCampaign campaign = item.payload<SugarCampaign>();

    if ((role == Qt::DisplayRole) || (role == Qt::EditRole)) {
        switch (columnTypes().at(column)) {
        case CampaignName:
            return campaign.name();
        case Status:
            return campaign.status();
        case Type:
            return campaign.campaignType();
        case EndDate:
            return campaign.endDate();
        case User:
            return campaign.assignedUserName();
        default:
            return QVariant();
        }
    }
    return QVariant();
}

/**
 * Return the data. KContacts::Addressee type - ref: Contacts
 * Only called for DisplayRole, EditRole and FontRole
 */
QVariant ItemsTreeModel::contactData(const Item &item, int column, int role) const
{
    if (!item.hasPayload<KContacts::Addressee>()) {

        // Pass modeltest
        if (role == Qt::DisplayRole) {
            return item.remoteId();
        }

        return QVariant();
    }

    const KContacts::Addressee addressee = item.payload<KContacts::Addressee>();
    const SugarContactWrapper contactWrapper(addressee);

    if ((role == Qt::DisplayRole) || (role == Qt::EditRole)) {
        switch (columnTypes().at(column)) {
        case FullName:
            return addressee.assembledName();
        case Title:
            return addressee.title();
        case Account:
            // not using addressee.organization() since that doesn't follow account renames/deletions
            return ReferencedData::instance(AccountRef)->referencedData(contactWrapper.accountId());
        case PreferredEmail:
            return addressee.preferredEmail();
        case PhoneWork:
            return addressee.phoneNumber(KContacts::PhoneNumber::Work).number();
        case PhoneMobile:
            return addressee.phoneNumber(KContacts::PhoneNumber::Cell).number();
        case Country:
            return countryForContact(addressee);
        case CreationDate: {
            const QDateTime dt = KDCRMUtils::dateTimeFromString(contactWrapper.dateCreated());
            if (role == Qt::DisplayRole)
                return KDCRMUtils::formatDate(dt.date());
            return dt; // for sorting
        }
        case LastModifiedDate:
        {
            const QDateTime dt = KDCRMUtils::dateTimeFromString(contactWrapper.dateModified());
            if (role == Qt::DisplayRole)
                return KDCRMUtils::formatDate(dt.date());
            return dt; // for sorting
        }
        case NumberOfOpportunities:
        {
            return mLinkedItemsRepository->opportunitiesForAccount(contactWrapper.accountId()).count();
        }
        case NumberOfDocumentsNotesEmails:
        {
            const QString accountId = contactWrapper.accountId();
            // The goal is to find those with 0 of each (for GDPR cleanup purposes)
            // so I'm not doing 3 different columns, for now.
            return mLinkedItemsRepository->documentsForAccount(accountId).count() +
                    mLinkedItemsRepository->notesForAccount(accountId).count() +
                    mLinkedItemsRepository->emailsForAccount(accountId).count();
        }
        case LeadSource:
            return contactWrapper.leadSource();
        case AssignedTo:
            return contactWrapper.assignedUserName();
        default:
            return QVariant();
        }
    }

    if (role == Qt::FontRole) {
        switch (columnTypes().at(column)) {
        case PreferredEmail:
            if (contactWrapper.invalidEmail() == QLatin1String("1")) {
                QFont f;
                f.setStrikeOut(true);
                return f;
            }
            break;
        default:
            break;
        }
    }

    return QVariant();
}

/**
 * Return the data. SugarLead type
 */
QVariant ItemsTreeModel::leadData(const Item &item, int column, int role) const
{
    if (!item.hasPayload<SugarLead>()) {

        // Pass modeltest
        if (role == Qt::DisplayRole) {
            return item.remoteId();
        }

        return QVariant();
    }

    const SugarLead lead = item.payload<SugarLead>();

    if ((role == Qt::DisplayRole) || (role == Qt::EditRole)) {
        switch (columnTypes().at(column)) {
        case LeadName:
            return lead.lastName();
        case LeadStatus:
            return lead.status();
        case LeadAccountName:
            return lead.accountName();
        case LeadEmail:
            return lead.email1();
        case LeadUser:
            return lead.assignedUserName();
        default:
            return QVariant();
        }
    }
    return QVariant();
}

/**
 * Return the data. SugarOpportunity type
 * Only called for DisplayRole, EditRole and FontRole
 */
QVariant ItemsTreeModel::opportunityData(const Item &item, int column, int role) const
{
    if (!item.hasPayload<SugarOpportunity>()) {

        // Pass modeltest
        if (role == Qt::DisplayRole) {
            return item.remoteId();
        }

        return QVariant();
    }

    const SugarOpportunity opportunity = item.payload<SugarOpportunity>();

    if ((role == Qt::DisplayRole) || (role == Qt::EditRole)) {
        switch (columnTypes().at(column)) {
        case OpportunityName:
            return opportunity.name();
        case OpportunityAccountName: {
            QString name = ReferencedData::instance(AccountRef)->referencedData(opportunity.accountId());
            if (name.isEmpty()) {
                name = "ERROR: unknown account! ID=" + opportunity.accountId() + "  tempAccountName=" + opportunity.tempAccountName();
            }
            return name;
        }
        case SalesStage:
            return opportunity.salesStage();
        case Probability: {
            const int probability = opportunity.probability().toInt();
            if (role == Qt::DisplayRole) {
                return i18nc("Probability percentage of opportunity", "%1 %", probability);
            }
            return probability; // for sorting
        }
        case Amount: {
            const double amount = QLocale::c().toDouble(opportunity.amount());
            if (role == Qt::DisplayRole) {
                // We can't use opportunity.currencySymbol(), it's only set for euros.
                // So we have to look up currencyId() in the (fake) enum definition set by the resource.
                const auto &enums = mCollectionManager->enumDefinitions(item.parentCollection().id());
                const int pos = enums.indexOf(KDCRMFields::currencyId());
                if (pos > -1) {
                    const auto &enumDef = enums.at(pos);
                    const QString currencySymbol = enumDef.value(opportunity.currencyId());
                    if (!currencySymbol.isEmpty()) {
                        return QLocale().toCurrencyString(amount, currencySymbol);
                    }
                }
            }
            return amount; // for sorting
        }
        case OpportunitySize:
            return opportunity.opportunitySize();
        case OpportunityPriority: {
            const QString priority = opportunity.opportunityPriority().toUpper();
            if (role == Qt::DisplayRole)
                return priority;
            return priority.isEmpty() ? QString::fromLatin1("Z") : priority; // for better ordering: A, B, C, <none>
        }
        case Description:
            return opportunity.limitedDescription(1);
        case CloseDate: {
            const QDate date = KDCRMUtils::dateFromString(opportunity.dateClosed());
            if (role == Qt::DisplayRole)
                return KDCRMUtils::formatDate(date);
            return date; // for sorting
        }
        case CreationDate: {
            const QDateTime dt = KDCRMUtils::dateTimeFromString(opportunity.dateEntered());
            if (role == Qt::DisplayRole)
                return KDCRMUtils::formatDate(dt.date());
            return dt; // for sorting
        }
        case NextStepDate:
            if (role == Qt::DisplayRole)
                return KDCRMUtils::formatDate(opportunity.nextCallDate());
            return opportunity.nextCallDate(); // for sorting
        case NextStep:
            return opportunity.nextStep();
        case LastModifiedDate: {
            const QDateTime dt = opportunity.dateModified();
            if (role == Qt::DisplayRole)
                return KDCRMUtils::formatDate(dt.date());
            return dt; // for sorting
        }
        case AssignedTo:
            return opportunity.assignedUserName();
        case PostalCode:
            return AccountRepository::instance()->accountById(opportunity.accountId()).postalCodeForGui();
        case City:
            return AccountRepository::instance()->accountById(opportunity.accountId()).cityForGui();
        case Country:
            return AccountRepository::instance()->accountById(opportunity.accountId()).countryForGui();

        default:
            return QVariant();
        }
    }
    if (role == Qt::FontRole) {
        if (opportunity.customFields().value(QStringLiteral("urgent")) == QLatin1String("1")) {
            QFont boldFont;
            boldFont.setBold(true);
            return boldFont;
        }
    }

    return QVariant();
}

QVariant ItemsTreeModel::accountToolTip(const Item &item) const
{
    if (!item.hasPayload<SugarAccount>()) {
        return QVariant();
    }

    const SugarAccount account = item.payload<SugarAccount>();
    QString accountDescription = account.limitedDescription(5);
    accountDescription.replace('\n', QLatin1String("<br>"));

    QString toolTipOutput;
    QString accountCountry = account.countryForGui();

    if (!account.name().isEmpty()) {
        toolTipOutput.append(i18n("<p><b>Name</b><br>%1</p>", account.name()));

        if (!accountCountry.isEmpty()) {
            toolTipOutput.insert(toolTipOutput.length()-4, i18n(" (%1)", accountCountry));
        }

    } else if (!accountCountry.isEmpty()) {
        toolTipOutput.append(i18n("<p><b>Country</b><br>%1</p>", accountCountry));
    }

    QDateTime dateModified = KDCRMUtils::dateTimeFromString(account.dateModifiedRaw());
    if (dateModified.isValid()) {
        toolTipOutput.append(i18n("<p><b>Last Modified</b><br>%1 (%2)</p>", dateModified.toString(Qt::SystemLocaleShortDate), account.modifiedByName()));
    }

    if (!accountDescription.isEmpty()) {
        toolTipOutput.append(i18n("<p><b>Description</b><br>%1</p>", accountDescription));
    }

    return toolTipOutput;
}

QVariant ItemsTreeModel::opportunityToolTip(const Item &item) const
{
    if (!item.hasPayload<SugarOpportunity>()) {
        return QVariant();
    }

    const SugarOpportunity opportunity = item.payload<SugarOpportunity>();

    QString toolTipOutput;

    if (!opportunity.name().isEmpty()) {
        toolTipOutput.append(i18n("<p><b>Name</b><br>%1</p>", opportunity.name()));
    }

    QDateTime dateModified = opportunity.dateModified();
    if (dateModified.isValid()) {
        toolTipOutput.append(i18n("<p><b>Last Modified</b><br>%1 (%2)</p>", dateModified.toString(Qt::SystemLocaleShortDate), opportunity.modifiedByName()));
    }

    if (!opportunity.nextStep().isEmpty()) {
        toolTipOutput.append(i18n("<p><b>Next Step</b><br>%1 (%2)</p>", opportunity.nextStep(), opportunity.nextCallDate().toString(Qt::SystemLocaleShortDate)));
    }

    QString opportunityShortDescription = opportunity.limitedDescription(5);
    opportunityShortDescription.replace('\n', QLatin1String("<br>"));
    if (!opportunityShortDescription.isEmpty()) {
        toolTipOutput.append(i18n("<p><b>Description</b><br>%1</p>", opportunityShortDescription));
    }

    return toolTipOutput;
}

ItemsTreeModel::ColumnTypes ItemsTreeModel::columnTypes(DetailsType type)
{
    switch (type) {
    case DetailsType::Account:
        return {
            ItemsTreeModel::Name,
            ItemsTreeModel::Street,
            ItemsTreeModel::City,
            ItemsTreeModel::PostalCode,
            ItemsTreeModel::Country,
            ItemsTreeModel::Phone,
            ItemsTreeModel::Email,
            ItemsTreeModel::CreatedBy,
            ItemsTreeModel::NumberOfOpportunities,
            ItemsTreeModel::NumberOfContacts,
            ItemsTreeModel::NumberOfDocumentsNotesEmails,
        };
    case DetailsType::Contact:
        return {
            ItemsTreeModel::FullName,
            ItemsTreeModel::Title,
            ItemsTreeModel::Account,
            ItemsTreeModel::Country,
            ItemsTreeModel::PreferredEmail,
            ItemsTreeModel::PhoneWork,
            ItemsTreeModel::PhoneMobile,
            ItemsTreeModel::CreationDate,
            ItemsTreeModel::LastModifiedDate,
            ItemsTreeModel::LeadSource,
            ItemsTreeModel::AssignedTo,
            ItemsTreeModel::NumberOfOpportunities,
            ItemsTreeModel::NumberOfDocumentsNotesEmails,
        };
    case DetailsType::Lead:
        return {
            ItemsTreeModel::LeadName,
            ItemsTreeModel::LeadStatus,
            ItemsTreeModel::LeadAccountName,
            ItemsTreeModel::LeadEmail,
            ItemsTreeModel::LeadUser
        };
        break;
    case DetailsType::Opportunity:
        return {
            ItemsTreeModel::OpportunityAccountName,
            ItemsTreeModel::OpportunityName,
            ItemsTreeModel::PostalCode,
            ItemsTreeModel::City,
            ItemsTreeModel::Country,
            ItemsTreeModel::SalesStage,
            ItemsTreeModel::Probability,
            ItemsTreeModel::Amount,
            ItemsTreeModel::OpportunitySize,
            ItemsTreeModel::Description,
            ItemsTreeModel::CloseDate,
            ItemsTreeModel::CreationDate,
            ItemsTreeModel::NextStep,
            ItemsTreeModel::NextStepDate,
            ItemsTreeModel::LastModifiedDate,
            ItemsTreeModel::AssignedTo,
            ItemsTreeModel::OpportunityPriority,
        };
    case DetailsType::Campaign:
        return {
            ItemsTreeModel::CampaignName,
            ItemsTreeModel::Status,
            ItemsTreeModel::Type,
            ItemsTreeModel::EndDate,
            ItemsTreeModel::User
        };
    }

    Q_UNREACHABLE();
    return {};
}

QString ItemsTreeModel::columnTitle(ItemsTreeModel::ColumnType col) const
{
    switch (col) {
    case Name:
        return i18nc("@title:column name", "Name");
    case Street:
        return i18nc("@title:column street", "Street");
    case PostalCode:
        return i18nc("@title:column postalcode", "Postal Code");
    case City:
        return i18nc("@title:column city", "City");
    case Country:
        return i18nc("@title:column country ", "Country");
    case Phone:
        return i18nc("@title:column phone", "Phone");
    case Email:
        return i18nc("@title:column email", "Email");
    case CloseDate:
        return i18nc("@title:column date closed", "Close Date");
    case CreationDate:
        return i18nc("@title:column date created", "Creation Date");
    case CreatedBy:
        return i18nc("@title:column created by user", "Created By");
    case CampaignName:
        return i18nc("@title:column name of a campaign ", "Campaign");
    case Status:
        return i18nc("@title:column status - status", "Status");
    case Type:
        return i18nc("@title:column type - Campaign type", "Type");
    case EndDate:
        return i18nc("@title:column end date - End Date", "End Date");
    case User:
        return i18nc("@title:column Assigned User Name", "User");
    case FullName:
        return i18nc("@title:column full name of a contact ", "Name");
    case Title:
        return i18nc("@title:column contact title", "Title");
    case Account:
        return i18nc("@title:column company", "Account");
    case PreferredEmail:
        return i18nc("@title:column email", "Preferred Email");
    case PhoneWork:
        return i18nc("@title:column phone (work)", "Office Phone");
    case PhoneMobile:
        return i18nc("@title:column phone (mobile)", "Mobile");
    case LeadName:
        return i18nc("@title:column Lead's Full Name", "Name");
    case LeadStatus:
        return i18nc("@title:column Lead's Status", "Status");
    case LeadAccountName:
        return i18nc("@title:column Account Name", "Account Name");
    case LeadEmail:
        return i18nc("@title:column Lead's Primary email", "Email");
    case LeadUser:
        return i18nc("@title:column Lead's Assigny name", "User");
    case OpportunityName:
        return i18nc("@title:column name for the Opportunity", "Opportunity");
    case OpportunityAccountName:
        return i18nc("@title:column account name", "Account Name");
    case OpportunitySize:
        return i18nc("@title:column opportunity size (abbreviated)", "S");
    case OpportunityPriority:
        return i18nc("@title:column opportunity priority (abbreviated)", "P");
    case SalesStage:
        return i18nc("@title:column sales stage", "Sales Stage");
    case Probability:
        return i18nc("@title:column probability", "Probability");
    case Amount:
        return i18nc("@title:column amount", "Amount");
    case Description:
        return i18nc("@title:column description", "Description");
    case NextStep:
        return i18nc("@title:column next step for an opportunity", "Next Step");
    case NextStepDate:
        return i18nc("@title:column date of the next step for an opportunity", "Next Step Date");
    case LastModifiedDate:
        return i18nc("@title:column", "Last Modified Date");
    case AssignedTo:
        return i18nc("@title:column name of the person this object is assigned to", "Assigned To");
    case LeadSource:
        return i18nc("@title:column lead source", "Lead source");
    case NumberOfOpportunities:
        return i18nc("@title:column number of opportunities for this account (or this contact's account)", "# Opps");
    case NumberOfContacts:
        return i18nc("@title:column number of contacts for this account", "# Contacts");
    case NumberOfDocumentsNotesEmails:
        return i18nc("@title:column number of documents + notes + emails for this account", "# Docs/Notes/Emails");
    }
    return QString();
}

QString ItemsTreeModel::columnToolTip(ItemsTreeModel::ColumnType col) const
{
    switch (col) {
    case OpportunitySize:
        return i18nc("@title:column opportunity size", "Size");
    case OpportunityPriority:
        return i18nc("@title:column opportunity priority", "Priority");
    default:
        return {};
    }
}

QString ItemsTreeModel::columnNameFromType(ItemsTreeModel::ColumnType col)
{
    const QMetaObject &mo = staticMetaObject;
    int index = mo.indexOfEnumerator("ColumnType");
    Q_ASSERT(index >= 0);
    QMetaEnum metaEnum = mo.enumerator(index);
    return metaEnum.valueToKey(col);
}

ItemsTreeModel::ColumnType ItemsTreeModel::columnTypeFromName(const QString &name)
{
    const QMetaObject &mo = staticMetaObject;
    int index = mo.indexOfEnumerator("ColumnType");
    Q_ASSERT(index >= 0);
    QMetaEnum metaEnum = mo.enumerator(index);
    return static_cast<ColumnType>(metaEnum.keyToValue(name.toLatin1().constData()));
}

ItemsTreeModel::ColumnTypes ItemsTreeModel::defaultVisibleColumns() const
{
    ItemsTreeModel::ColumnTypes columns = columnTypes();
    switch (mType) {
    case DetailsType::Account:
        columns.removeAll(ItemsTreeModel::Street);
        columns.removeAll(ItemsTreeModel::CreatedBy);
        columns.removeAll(ItemsTreeModel::NumberOfOpportunities);
        columns.removeAll(ItemsTreeModel::NumberOfContacts);
        columns.removeAll(ItemsTreeModel::NumberOfDocumentsNotesEmails);
        break;
    case DetailsType::Contact:
        // too wide and too seldom filled in
        columns.removeAll(ItemsTreeModel::Title);
        columns.removeAll(ItemsTreeModel::CreationDate);
        columns.removeAll(ItemsTreeModel::LastModifiedDate);
        columns.removeAll(ItemsTreeModel::NumberOfOpportunities);
        columns.removeAll(ItemsTreeModel::AssignedTo);
        columns.removeAll(ItemsTreeModel::LeadSource);
        break;
    case DetailsType::Lead:
        break;
    case DetailsType::Opportunity:
        columns.removeAll(ItemsTreeModel::Probability);
        columns.removeAll(ItemsTreeModel::PostalCode);
        columns.removeAll(ItemsTreeModel::City);
        columns.removeAll(ItemsTreeModel::Amount);
        columns.removeAll(ItemsTreeModel::NextStep);
        columns.removeAll(ItemsTreeModel::CloseDate);
        columns.removeAll(ItemsTreeModel::LastModifiedDate);
        columns.removeAll(ItemsTreeModel::OpportunitySize);
        break;
    case DetailsType::Campaign:
        break;
    default:
        break;
    }
    return columns;
}

QString ItemsTreeModel::columnName(int column) const
{
    return columnNameFromType(d->mColumns.at(column));
}

