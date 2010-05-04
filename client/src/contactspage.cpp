#include "contactspage.h"

#include <akonadi/contact/contactstreemodel.h>

#include <akonadi/agentmanager.h>
#include <akonadi/changerecorder.h>
#include <akonadi/collection.h>
#include <akonadi/collectionfetchjob.h>
#include <akonadi/collectionfetchscope.h>
#include <akonadi/collectionstatistics.h>
#include <akonadi/entitymimetypefiltermodel.h>
#include <akonadi/item.h>
#include <akonadi/itemcreatejob.h>
#include <akonadi/itemdeletejob.h>
#include <akonadi/itemfetchscope.h>
#include <akonadi/itemmodifyjob.h>

#include <kabc/addressee.h>

using namespace Akonadi;

ContactsPage::ContactsPage( QWidget *parent )
    : QWidget( parent ),
      mChangeRecorder( new ChangeRecorder( this ) )
{
    mUi.setupUi( this );
    initialize();
}

ContactsPage::~ContactsPage()
{
}

void ContactsPage::slotResourceSelectionChanged( const QByteArray &identifier )
{
    if ( mContactsCollection.isValid() ) {
        mChangeRecorder->setCollectionMonitored( mContactsCollection, false );
    }

    /*
     * Look for the "Contacts" collection explicitly by listing all collections
     * of the currently selected resource, filtering by MIME type.
     * include statistics to get the number of items in each collection
     */
    CollectionFetchJob *job = new CollectionFetchJob( Collection::root(), CollectionFetchJob::Recursive );
    job->fetchScope().setResource( identifier );
    job->fetchScope().setContentMimeTypes( QStringList() << KABC::Addressee::mimeType() );
    job->fetchScope().setIncludeStatistics( true );
    connect( job, SIGNAL( result( KJob* ) ),
             this, SLOT( slotCollectionFetchResult( KJob* ) ) );

}

void ContactsPage::slotCollectionFetchResult( KJob *job )
{
    CollectionFetchJob *fetchJob = qobject_cast<CollectionFetchJob*>( job );

    // look for the "Contacts" collection
    Q_FOREACH( const Collection &collection, fetchJob->collections() ) {
        if ( collection.remoteId() == QLatin1String( "Contacts" ) ) {
            mContactsCollection = collection;
            break;
        }
    }

    if ( mContactsCollection.isValid() ) {
        mUi.newContactPB->setEnabled( true );
        mChangeRecorder->setCollectionMonitored( mContactsCollection, true );

        // if empty, the collection might not have been loaded yet, try synchronizing
        if ( mContactsCollection.statistics().count() == 0 ) {
            AgentManager::self()->synchronizeCollection( mContactsCollection );
        }
    } else {
        mUi.newContactPB->setEnabled( false );
    }
}

void ContactsPage::slotContactChanged( const Item &item )
{
    qDebug() << "Sorry, ContactsPage::slotContactChanged, NIY";

    /*
    if ( item.isValid() && item.hasPayload<KABC::Addressee>() ) {
        const KABC::Addressee addressee = item.payload<KABC::Addressee>();

        mUi.firstName->setText( addressee.givenName() );
        mUi.lastName->setText( addressee.familyName() );
        mUi.title->setText( addressee.title() );
        mUi.company->setText( addressee.organization() );
        mUi.department->setText( addressee.department() );

        mUi.modifyContactButton->setEnabled( true );
        mUi.removeContactButton->setEnabled( true );
    } else {
        mUi.firstName->clear();
        mUi.lastName->clear();
        mUi.title->clear();
        mUi.company->clear();
        mUi.department->clear();

        mUi.modifyContactButton->setEnabled( false );
        mUi.removeContactButton->setEnabled( false );
    }
    */
}

void ContactsPage::slotNewContactClicked()
{
    qDebug() << "Sorry, ContactsPage::slotNewContactClicked(), NIY";
}

void ContactsPage::slotFilterChanged( const QString& filterText )
{
    qDebug() << "Sorry, ContactsPage::slotFilterChanged(), NIY";
}

void ContactsPage::initialize()
{
    QStringList filtersLabels;
    filtersLabels << QString( "All Contacts" )
                  << QString( "Birthdays this month" );
    mUi.filtersCB->addItems( filtersLabels );
    mUi.contactsTV->header()->setResizeMode( QHeaderView::ResizeToContents );

    connect( mUi.newContactPB, SIGNAL( clicked() ),
             this, SLOT( slotNewContactClicked() ) );
    connect( mUi.filtersCB, SIGNAL( currentIndexChanged( const QString& ) ),
             this,  SLOT( slotFilterChanged( const QString& ) ) );

    // automatically get the full data when items change
    mChangeRecorder->itemFetchScope().fetchFullPayload( true );

    /*
     * convenience model for contacts, allowing us to easily specify the columns
     * to show
     * could use an Akonadi::ItemModel instead because we don't have a tree of
     * collections but only a single one
     */
    ContactsTreeModel *contactsModel = new ContactsTreeModel( mChangeRecorder, this );

    ContactsTreeModel::Columns columns;
    columns << ContactsTreeModel::FullName
            << ContactsTreeModel::Role
            << ContactsTreeModel::Organization
            << ContactsTreeModel::PreferredEmail
            << ContactsTreeModel::PhoneNumbers
            << ContactsTreeModel::GivenName;
    contactsModel->setColumns( columns );

    // same as for the ContactsTreeModel, not strictly necessary
    EntityMimeTypeFilterModel *filterModel = new EntityMimeTypeFilterModel( this );
    filterModel->setSourceModel( contactsModel );
    filterModel->addMimeTypeInclusionFilter( KABC::Addressee::mimeType() );
    filterModel->setHeaderGroup( EntityTreeModel::ItemListHeaders );

    mUi.contactsTV->setModel( filterModel );

    connect( mUi.contactsTV, SIGNAL( currentChanged( Akonadi::Item ) ), this, SLOT( slotContactChanged( Akonadi::Item ) ) );

}
