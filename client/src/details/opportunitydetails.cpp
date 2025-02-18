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

#include "opportunitydetails.h"

#include "ui_opportunitydetails.h"
#include "clientsettings.h"
#include "documentswindow.h"
#include "enums.h"
#include "itemstreemodel.h"
#include "linkeditemsrepository.h"
#include "modelrepository.h"
#include "noteswindow.h"
#include "opportunitydataextractor.h"
#include "referenceddatamodel.h"
#include "selectitemdialog.h"
#include "externalopen.h"

#include "kdcrmdata/kdcrmutils.h"
#include "kdcrmdata/kdcrmfields.h"
#include "kdcrmdata/sugaropportunity.h"

#include <KLocalizedString>
#include "fatcrm_client_debug.h"

#include <QCalendarWidget>
#include <QCompleter>
#include <QMenu>
#include <referenceddata.h>
#include <sugarcontactwrapper.h>

OpportunityDetails::OpportunityDetails(QWidget *parent)
    : Details(DetailsType::Opportunity, parent), mUi(new Ui::OpportunityDetails), mDataExtractor(new OpportunityDataExtractor)
{
    mUi->setupUi(this);

    mUi->urllabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    mUi->next_call_date->setNullable(true);

    mUi->name->setObjectName(KDCRMFields::name());
    mUi->account_id->setObjectName(KDCRMFields::accountId());
    mUi->opportunity_type->setObjectName(KDCRMFields::opportunityType());
    mUi->lead_source->setObjectName(KDCRMFields::leadSource());
    mUi->date_closed->setObjectName(KDCRMFields::dateClosed());
    mUi->assigned_user_id->setObjectName(KDCRMFields::assignedUserId());
    mUi->amount->setObjectName(KDCRMFields::amount());
    mUi->next_step->setObjectName(KDCRMFields::nextStep());
    mUi->next_call_date->setObjectName(KDCRMFields::nextCallDate());
    mUi->sales_stage->setObjectName(KDCRMFields::salesStage());
    mUi->probability->setObjectName(KDCRMFields::probability());
    mUi->opportunityPriority->setObjectName(KDCRMFields::opportunityPriority());
    mUi->opportunitySize->setObjectName(KDCRMFields::opportunitySize());
    initialize();
}

OpportunityDetails::~OpportunityDetails()
{
    delete mUi;
}

void OpportunityDetails::setLinkedItemsRepository(LinkedItemsRepository *repo)
{
    mLinkedItemsRepository = repo;
    connect(mLinkedItemsRepository, &LinkedItemsRepository::opportunityModified,
            this, &OpportunityDetails::slotLinkedItemsModified);
}

void OpportunityDetails::initialize()
{
    ReferencedDataModel::setModelForCombo(mUi->account_id, AccountRef);
    ReferencedDataModel::setModelForCombo(mUi->assigned_user_id, AssignedToRef);

    connect(mUi->assigned_user_id, &QComboBox::currentTextChanged, this, [this](const QString &currentText) {
        mUi->assignToMeButton->setEnabled(currentText != ClientSettings::self()->fullUserName());
    });
    connect(mUi->assignToMeButton, &QPushButton::clicked, this, [this]() {
        mUi->assigned_user_id->setCurrentText(ClientSettings::self()->fullUserName());
    });
    connect(mUi->buttonSelectAccount, &QAbstractButton::clicked, this, &OpportunityDetails::slotSelectAccount);
    connect(mUi->nextStepDateAutoButton, &QAbstractButton::clicked, this, &OpportunityDetails::slotAutoNextStepDate);
    connect(mUi->sales_stage, SIGNAL(activated(QString)),
            this, SLOT(slotSalesStageActivated(QString)));
    connect(mUi->date_closed, &KDateComboBox::dateChanged, this, &OpportunityDetails::slotCloseDateChanged);
    connect(mUi->buttonOpenAccount, &QAbstractButton::clicked, this, &OpportunityDetails::slotOpenAccount);
    connect(mUi->account_id, &QComboBox::currentTextChanged, this, [this](const QString &currentText) {
        mUi->buttonOpenAccount->setEnabled(!currentText.isEmpty());
    });
    connect(mUi->buttonOpenContact, &QAbstractButton::clicked, this, &OpportunityDetails::slotOpenContact);
    connect(mUi->primary_contact_id, &QComboBox::currentTextChanged, this, [this](const QString &currentText) {
        mUi->buttonOpenContact->setEnabled(!currentText.isEmpty());
    });
}

ItemDataExtractor *OpportunityDetails::itemDataExtractor() const
{
    return mDataExtractor.get();
}

void OpportunityDetails::slotAutoNextStepDate()
{
    mUi->next_call_date->setDate(QDate::currentDate().addDays(14));
}

void OpportunityDetails::slotSalesStageActivated(const QString &stage)
{
    int percent = 50;
    if (stage == QLatin1String("Prospecting"))
        percent = 10;
    else if (stage == QLatin1String("Proposal/Price Quote"))
        percent = 65;
    else if (stage == QLatin1String("Negotiation/Review"))
        percent = 80;
    else if (stage == QLatin1String("Closed Won"))
        percent = 100;
    else if (stage == QLatin1String("Closed Lost"))
        percent = 0;
    mUi->probability->setValue(percent);

    if (stage == QLatin1String("Closed Won") || stage == QLatin1String("Closed Lost")) {
        updateCloseDateLabel(true);
        if (!mCloseDateChangedByUser) {
            mUi->date_closed->setDate(QDate::currentDate());
            mCloseDateChangedByUser = false;
        }
    } else {
        updateCloseDateLabel(false);
        if (!mCloseDateChangedByUser) {
            mUi->date_closed->setDate(mOriginalCloseDate);
            mCloseDateChangedByUser = false;
        }
    }
}

void OpportunityDetails::slotCloseDateChanged(const QDate &newCloseDate)
{
    mCloseDateChangedByUser = (newCloseDate != mOriginalCloseDate);
}

void OpportunityDetails::slotLinkedItemsModified(const QString &oppId)
{
    if (oppId == id()) {
        updateLinkedItemsButtons();
    }
}

QMap<QString, QString> OpportunityDetails::data(const Akonadi::Item &item) const
{
    Q_ASSERT(item.isValid());
    Q_ASSERT(item.hasPayload<SugarOpportunity>());
    SugarOpportunity opportunity = item.payload<SugarOpportunity>();
    return opportunity.data();
}

void OpportunityDetails::updateItem(Akonadi::Item &item, const QMap<QString, QString> &data) const
{
    SugarOpportunity opportunity;
    if (item.hasPayload<SugarOpportunity>()) {
        opportunity = item.payload<SugarOpportunity>();
    }
    opportunity.setData(data);

    item.setMimeType(SugarOpportunity::mimeType());
    item.setPayload<SugarOpportunity>(opportunity);
}

void OpportunityDetails::updateLinkedItemsButtons()
{
    const QString oppId = id();
    {
        const int notes = oppId.isEmpty() ? 0 : mLinkedItemsRepository->notesForOpportunity(oppId).count() + mLinkedItemsRepository->emailsForOpportunity(oppId).count();
        const QString buttonText = (notes == 0) ? i18n("Add Note") : i18np("View 1 Note", "View %1 Notes", notes);
        mUi->viewNotesButton->setText(buttonText);
        mUi->viewNotesButton->setEnabled(!oppId.isEmpty());
    }
    {
        const auto documents = mLinkedItemsRepository->documentsForOpportunity(oppId);
        const QString buttonText = documents.isEmpty() ? i18n("Attach Document") : i18np("Manage 1 Document", "Manage %1 Documents", documents.count());
        mUi->manageDocumentsButton->setText(buttonText);
        mUi->viewDocumentsButton->setEnabled(!documents.isEmpty());

        auto *menu = new QMenu(this);
        for (const auto &doc : documents) {
            auto *action = menu->addAction(doc.documentName().toHtmlEscaped());
            const auto docId = doc.documentRevisionId();
            connect(action, &QAction::triggered, this, [this, docId]() {
                ExternalOpen::openSugarDocument(docId, resourceIdentifier(), this);
            });
        }
        if (auto oldMenu = mUi->viewDocumentsButton->menu()) {
            oldMenu->deleteLater();
        }
        mUi->viewDocumentsButton->setMenu(menu);
    }
}

void OpportunityDetails::setDataInternal(const QMap<QString, QString> &data)
{
    fillComboBox(mUi->opportunity_type, KDCRMFields::opportunityType());
    fillComboBox(mUi->lead_source, KDCRMFields::leadSource());
    fillComboBox(mUi->sales_stage, KDCRMFields::salesStage());
    fillComboBox(mUi->opportunityPriority, KDCRMFields::opportunityPriority());
    fillComboBox(mUi->opportunitySize, KDCRMFields::opportunitySize());
    fillComboBox(mUi->currency_id, KDCRMFields::currencyId());

    auto contactsData = new ReferencedData(ReferencedDataType::ContactRef, this);
    const QVector<KContacts::Addressee> contacts = mLinkedItemsRepository->contactsForAccount(data.value(KDCRMFields::accountId()));
    QMap<QString, QString> contactIdName;
    for (const KContacts::Addressee &contact : contacts) {
        const SugarContactWrapper wrapper(contact);
        contactIdName.insert(wrapper.id(), wrapper.fullName());
    }
    contactsData->addMap(contactIdName, false);
    ReferencedDataModel::setModelForCombo(mUi->primary_contact_id, contactsData);

    const QUrl url = itemDataExtractor()->itemUrl(resourceBaseUrl(), id());
    if (url.isValid())
        mUi->urllabel->setText(QStringLiteral("<a href=\"%1\">Open Opportunity in Web Browser</a>").arg(url.toString()));
    else
        mUi->urllabel->clear();

    updateLinkedItemsButtons();

    mOriginalCloseDate = KDCRMUtils::dateFromString(data.value(KDCRMFields::dateClosed()));

    const QString stage = data.value(KDCRMFields::salesStage());
    updateCloseDateLabel(stage == QLatin1String("Closed Won") || stage == QLatin1String("Closed Lost"));
}

void OpportunityDetails::updateCloseDateLabel(bool closed)
{
    if (closed)
        mUi->expectedCloseDateLabel->setText(i18n("Close Date:"));
    else
        mUi->expectedCloseDateLabel->setText(i18n("Expected Close Date:"));
}

void OpportunityDetails::on_viewNotesButton_clicked()
{
    const QString oppId = id();
    const QVector<SugarNote> notes = mLinkedItemsRepository->notesForOpportunity(oppId);
    qCDebug(FATCRM_CLIENT_LOG) << notes.count() << "notes found for opp" << oppId;
    const QVector<SugarEmail> emails = mLinkedItemsRepository->emailsForOpportunity(oppId);
    qCDebug(FATCRM_CLIENT_LOG) << emails.count() << "emails found for opp" << oppId;
    auto *dlg = new NotesWindow(nullptr);
    dlg->setResourceIdentifier(resourceIdentifier());
    dlg->setLinkedItemsRepository(mLinkedItemsRepository);
    dlg->setLinkedTo(oppId, type());
    dlg->setWindowTitle(i18n("Notes for opportunity %1", name()));
    foreach(const SugarNote &note, notes) {
        dlg->addNote(note);
    }
    foreach(const SugarEmail &email, emails) {
        dlg->addEmail(email);
    }
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void OpportunityDetails::on_manageDocumentsButton_clicked()
{
    const QString oppId = id();

    auto *dlg = new DocumentsWindow(nullptr);
    connect(dlg, &DocumentsWindow::documentsCreated, this, &OpportunityDetails::syncRequired);
    dlg->setWindowTitle(i18n("Documents for opportunity %1", name()));

    dlg->setResourceIdentifier(resourceIdentifier());
    dlg->setLinkedItemsRepository(mLinkedItemsRepository);
    dlg->loadDocumentsFor(oppId, DocumentsWindow::Opportunity);

    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void OpportunityDetails::slotOpenAccount()
{
    const QString accountId = currentAccountId();
    emit openObject(DetailsType::Account, accountId);
}

void OpportunityDetails::slotOpenContact()
{
    const QString contactId = mUi->primary_contact_id->itemData(mUi->primary_contact_id->currentIndex()).toString();
    if (!contactId.isEmpty()) {
        emit openObject(DetailsType::Contact, contactId);
    }
}

void OpportunityDetails::slotSelectAccount()
{
    auto *dlg = new SelectItemDialog(DetailsType::Account, this);
    dlg->setModel(ModelRepository::instance()->model(DetailsType::Account));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &SelectItemDialog::selectedItem, this, &OpportunityDetails::slotAccountSelected);
    dlg->show();
}

void OpportunityDetails::slotAccountSelected(const QString &accountId)
{
    const int idx = mUi->account_id->findData(accountId);
    if (idx > -1) {
        mUi->account_id->setCurrentIndex(idx);
    }
}

void OpportunityDetails::setItemsTreeModel(ItemsTreeModel *model)
{
    QStringList nextStepList;
    nextStepList.reserve(2500);
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        const Akonadi::Item item = index.data(Akonadi::EntityTreeModel::ItemRole).value<Akonadi::Item>();
        const SugarOpportunity opportunity = item.payload<SugarOpportunity>();
        const QString nextStep = opportunity.nextStep();
        if (!nextStep.isEmpty() && !nextStepList.contains(nextStep))
            nextStepList.append(nextStep);
    }

    QCompleter *nextStepCompleter = new QCompleter(nextStepList, this);
    nextStepCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    mUi->next_step->setCompleter(nextStepCompleter);
    Details::setItemsTreeModel(model);
}

