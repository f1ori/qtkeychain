/******************************************************************************
 *   Copyright (C) 2011 Frank Osterfeld <frank.osterfeld@gmail.com>           *
 *                                                                            *
 * This program is distributed in the hope that it will be useful, but        *
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY *
 * or FITNESS FOR A PARTICULAR PURPOSE. For licensing and distribution        *
 * details, check the accompanying file 'COPYING'.                            *
 *****************************************************************************/
#include "keychain_p.h"

#include <QSettings>

#include <auto_ptr.h>

using namespace QKeychain;

void ReadPasswordJobPrivate::scheduledStart() {
    if ( QDBusConnection::sessionBus().isConnected() )
    {
        iface = new org::kde::KWallet( QLatin1String("org.kde.kwalletd"), QLatin1String("/modules/kwalletd"), QDBusConnection::sessionBus(), this );
        const QDBusPendingReply<int> reply = iface->open( QLatin1String("kdewallet"), 0, q->service() );
        QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher( reply, this );
        connect( watcher, SIGNAL(finished(QDBusPendingCallWatcher*)), this, SLOT(kwalletOpenFinished(QDBusPendingCallWatcher*)) );
    }
    else
    {
        // D-Bus is not reachable so none can tell us something about KWalletd
        QDBusError err( QDBusError::NoServer, "D-Bus is not running" );
        fallbackOnError( err );
    }
}

void ReadPasswordJobPrivate::fallbackOnError(const QDBusError& err )
{
    std::auto_ptr<QSettings> local( !q->settings() ? new QSettings( q->service() ) : 0 );
    QSettings* actual = q->settings() ? q->settings() : local.get();
    WritePasswordJobPrivate::Mode mode;

    if ( q->insecureFallback() && actual->contains( dataKey() ) ) {

        mode = (WritePasswordJobPrivate::Mode)actual->value( typeKey() ).toInt();
        data = actual->value( dataKey() ).toByteArray();

        q->emitFinished();
    } else {
        if ( err.type() == QDBusError::ServiceUnknown ) //KWalletd not running
            q->emitFinishedWithError( NoBackendAvailable, tr("No keychain service available") );
        else
            q->emitFinishedWithError( OtherError, tr("Could not open wallet: %1; %2").arg( QDBusError::errorString( err.type() ), err.message() ) );
    }
}

const QString ReadPasswordJobPrivate::typeKey()
{
    return QString( "%1/type" ).arg( key );
}

const QString ReadPasswordJobPrivate::dataKey()
{
    return QString( "%1/data" ).arg( key );
}

void ReadPasswordJobPrivate::kwalletOpenFinished( QDBusPendingCallWatcher* watcher ) {
    watcher->deleteLater();
    const QDBusPendingReply<int> reply = *watcher;

    std::auto_ptr<QSettings> local( !q->settings() ? new QSettings( q->service() ) : 0 );
    QSettings* actual = q->settings() ? q->settings() : local.get();
    WritePasswordJobPrivate::Mode mode;

    if ( reply.isError() ) {
        fallbackOnError( reply.error() );
        return;
    }

    if ( actual->contains( dataKey() ) ) {
        // We previously stored data in the insecure QSettings, but now have KWallet available.
        // Do the migration

        data = actual->value( dataKey() ).toByteArray();
        mode = (WritePasswordJobPrivate::Mode)actual->value( typeKey() ).toInt();
        actual->remove( key );

        q->emitFinished();


        WritePasswordJob* j = new WritePasswordJob( q->service(), 0 );
        j->setSettings( q->settings() );
        j->setKey( key );
        j->setAutoDelete( true );
        if ( mode == WritePasswordJobPrivate::Binary )
            j->setBinaryData( data );
        else if ( mode == WritePasswordJobPrivate::Text )
            j->setTextData( QString::fromUtf8( data ) );
        else
            Q_ASSERT( false );

        j->start();

        return;
    }

    walletHandle = reply.value();

    if ( walletHandle < 0 ) {
        q->emitFinishedWithError( AccessDenied, tr("Access to keychain denied") );
        return;
    }

    const QDBusPendingReply<int> nextReply = iface->entryType( walletHandle, q->service(), key, q->service() );
    QDBusPendingCallWatcher* nextWatcher = new QDBusPendingCallWatcher( nextReply, this );
    connect( nextWatcher, SIGNAL(finished(QDBusPendingCallWatcher*)), this, SLOT(kwalletEntryTypeFinished(QDBusPendingCallWatcher*)) );
}

void ReadPasswordJobPrivate::kwalletEntryTypeFinished( QDBusPendingCallWatcher* watcher ) {
    watcher->deleteLater();
    if ( watcher->isError() ) {
        const QDBusError err = watcher->error();
        q->emitFinishedWithError( OtherError, tr("Could not determine data type: %1; %2").arg( QDBusError::errorString( err.type() ), err.message() ) );
        return;
    }

    const QDBusPendingReply<int> reply = *watcher;

    dataType = reply.value() == 1/*Password*/ ? Text : Binary;

    const QDBusPendingCall nextReply = dataType == Text
        ? QDBusPendingCall( iface->readPassword( walletHandle, q->service(), key, q->service() ) )
        : QDBusPendingCall( iface->readEntry( walletHandle, q->service(), key, q->service() ) );
    QDBusPendingCallWatcher* nextWatcher = new QDBusPendingCallWatcher( nextReply, this );
    connect( nextWatcher, SIGNAL(finished(QDBusPendingCallWatcher*)), this, SLOT(kwalletReadFinished(QDBusPendingCallWatcher*)) );
}

void ReadPasswordJobPrivate::kwalletReadFinished( QDBusPendingCallWatcher* watcher ) {
    watcher->deleteLater();
    if ( watcher->isError() ) {
        const QDBusError err = watcher->error();
        q->emitFinishedWithError( OtherError, tr("Could not read password: %1; %2").arg( QDBusError::errorString( err.type() ), err.message() ) );
        return;
    }

    if ( dataType == Binary ) {
        QDBusPendingReply<QByteArray> reply = *watcher;
        data = reply.value();
    } else {
        QDBusPendingReply<QString> reply = *watcher;
        data = reply.value().toUtf8();
    }
    q->emitFinished();
}

void WritePasswordJobPrivate::scheduledStart() {
    if ( QDBusConnection::sessionBus().isConnected() )
    {
        iface = new org::kde::KWallet( QLatin1String("org.kde.kwalletd"), QLatin1String("/modules/kwalletd"), QDBusConnection::sessionBus(), this );
        const QDBusPendingReply<int> reply = iface->open( QLatin1String("kdewallet"), 0, q->service() );
        QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher( reply, this );
        connect( watcher, SIGNAL(finished(QDBusPendingCallWatcher*)), this, SLOT(kwalletOpenFinished(QDBusPendingCallWatcher*)) );
    }
    else
    {
        // D-Bus is not reachable so none can tell us something about KWalletd
        QDBusError err( QDBusError::NoServer, "D-Bus is not running" );
        fallbackOnError( err );
    }
}

void WritePasswordJobPrivate::fallbackOnError(const QDBusError &err)
{
    std::auto_ptr<QSettings> local( !q->settings() ? new QSettings(  q->service() ) : 0 );
    QSettings* actual = q->settings() ? q->settings() : local.get();

    if ( q->insecureFallback() ) {
        if ( mode == Delete ) {
            actual->remove( key );
            actual->sync();

            q->emitFinished();
            return;
        }

        actual->setValue( QString( "%1/type" ).arg( key ), (int)mode );
        if ( mode == Text )
            actual->setValue( QString( "%1/data" ).arg( key ), textData.toUtf8() );
        else if ( mode == Binary )
            actual->setValue( QString( "%1/data" ).arg( key ), binaryData );
        actual->sync();

        q->emitFinished();
    } else {
        q->emitFinishedWithError( OtherError, tr("Could not open wallet: %1; %2").arg( QDBusError::errorString( err.type() ), err.message() ) );
    }
}

void WritePasswordJobPrivate::kwalletOpenFinished( QDBusPendingCallWatcher* watcher ) {
    watcher->deleteLater();
    QDBusPendingReply<int> reply = *watcher;

    std::auto_ptr<QSettings> local( !q->settings() ? new QSettings(  q->service() ) : 0 );
    QSettings* actual = q->settings() ? q->settings() : local.get();

    if ( reply.isError() ) {
        fallbackOnError( reply.error() );
        return;
    }

    if ( actual->contains( key ) )
    {
        // If we had previously written to QSettings, but we now have a kwallet available, migrate and delete old insecure data
        actual->remove( key );
        actual->sync();
    }

    const int handle = reply.value();

    if ( handle < 0 ) {
        q->emitFinishedWithError( AccessDenied, tr("Access to keychain denied") );
        return;
    }

    QDBusPendingReply<int> nextReply;

    if ( !textData.isEmpty() )
        nextReply = iface->writePassword( handle, q->service(), key, textData, q->service() );
    else if ( !binaryData.isEmpty() )
        nextReply = iface->writeEntry( handle, q->service(), key, binaryData, q->service() );
    else
        nextReply = iface->removeEntry( handle, q->service(), key, q->service() );

    QDBusPendingCallWatcher* nextWatcher = new QDBusPendingCallWatcher( nextReply, this );
    connect( nextWatcher, SIGNAL(finished(QDBusPendingCallWatcher*)), this, SLOT(kwalletWriteFinished(QDBusPendingCallWatcher*)) );
}

void WritePasswordJobPrivate::kwalletWriteFinished( QDBusPendingCallWatcher* watcher ) {
    watcher->deleteLater();
    QDBusPendingReply<int> reply = *watcher;
    if ( reply.isError() ) {
        const QDBusError err = reply.error();
        q->emitFinishedWithError( OtherError, tr("Could not open wallet: %1; %2").arg( QDBusError::errorString( err.type() ), err.message() ) );
        return;
    }

    q->emitFinished();
}
