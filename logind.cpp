/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2014 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
#include "logind.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusUnixFileDescriptor>
#include <QDBusMetaType>

#include <sys/stat.h>
#include <unistd.h>
#include "utils.h"

struct DBusLogindSeat {
    QString name;
    QDBusObjectPath path;
};

QDBusArgument &operator<<(QDBusArgument &argument, const DBusLogindSeat &seat)
{
    argument.beginStructure();
    argument << seat.name << seat.path ;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, DBusLogindSeat &seat)
{
    argument.beginStructure();
    argument >> seat.name >> seat.path;
    argument.endStructure();
    return argument;
}

Q_DECLARE_METATYPE(DBusLogindSeat)

namespace KWin
{

const static QString s_login1Service = QStringLiteral("org.freedesktop.login1");
const static QString s_login1Path = QStringLiteral("/org/freedesktop/login1");
const static QString s_login1ManagerInterface = QStringLiteral("org.freedesktop.login1.Manager");
const static QString s_login1SessionInterface = QStringLiteral("org.freedesktop.login1.Session");
const static QString s_dbusPropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

LogindIntegration *LogindIntegration::s_self = nullptr;

LogindIntegration *LogindIntegration::create(QObject *parent)
{
    Q_ASSERT(!s_self);
    s_self = new LogindIntegration(parent);
    return s_self;
}

LogindIntegration::LogindIntegration(const QDBusConnection &connection, QObject *parent)
    : QObject(parent)
    , m_bus(connection)
    , m_logindServiceWatcher(new QDBusServiceWatcher(s_login1Service,
                                                     m_bus,
                                                     QDBusServiceWatcher::WatchForUnregistration | QDBusServiceWatcher::WatchForRegistration,
                                                     this))
    , m_connected(false)
    , m_sessionControl(false)
    , m_sessionActive(false)
{
    connect(m_logindServiceWatcher, &QDBusServiceWatcher::serviceRegistered, this, &LogindIntegration::logindServiceRegistered);
    connect(m_logindServiceWatcher, &QDBusServiceWatcher::serviceUnregistered, this,
        [this]() {
            m_connected = false;
            emit connectedChanged();
        }
    );

    // check whether the logind service is registered
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("/"),
                                                          QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("ListNames"));
    QDBusPendingReply<QStringList> async = m_bus.asyncCall(message);
    QDBusPendingCallWatcher *callWatcher = new QDBusPendingCallWatcher(async, this);
    connect(callWatcher, &QDBusPendingCallWatcher::finished, this,
        [this](QDBusPendingCallWatcher *self) {
            QDBusPendingReply<QStringList> reply = *self;
            self->deleteLater();
            if (!reply.isValid()) {
                return;
            }
            if (reply.value().contains(s_login1Service)) {
                logindServiceRegistered();
            }
        }
    );
}

LogindIntegration::LogindIntegration(QObject *parent)
    : LogindIntegration(QDBusConnection::systemBus(), parent)
{
}

LogindIntegration::~LogindIntegration()
{
    s_self = nullptr;
}

void LogindIntegration::logindServiceRegistered()
{
    const QByteArray sessionId = qgetenv("XDG_SESSION_ID");
    QString methodName;
    QVariantList args;
    if (sessionId.isEmpty()) {
        methodName = QStringLiteral("GetSessionByPID");
        args << (quint32) QCoreApplication::applicationPid();
    } else {
        methodName = QStringLiteral("GetSession");
        args << QString::fromLocal8Bit(sessionId);
    }
    // get the current session
    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                          s_login1Path,
                                                          s_login1ManagerInterface,
                                                          methodName);
    message.setArguments(args);
    QDBusPendingReply<QDBusObjectPath> session = m_bus.asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(session, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this](QDBusPendingCallWatcher *self) {
            QDBusPendingReply<QDBusObjectPath> reply = *self;
            self->deleteLater();
            if (m_connected) {
                return;
            }
            if (!reply.isValid()) {
                qCDebug(KWIN_CORE) << "The session is not registered with logind" << reply.error().message();
                return;
            }
            m_sessionPath = reply.value().path();
            qCDebug(KWIN_CORE) << "Session path:" << m_sessionPath;
            m_connected = true;
            connectSessionPropertiesChanged();
            // activate the session, in case we are not on it
            QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                                m_sessionPath,
                                                                s_login1SessionInterface,
                                                                QStringLiteral("Activate"));
            // blocking on purpose
            m_bus.call(message);
            getSeat();
            getSessionActive();
            getVirtualTerminal();

            emit connectedChanged();
        }
    );
}

void LogindIntegration::connectSessionPropertiesChanged()
{
    m_bus.connect(s_login1Service,
                  m_sessionPath,
                  s_dbusPropertiesInterface,
                  QStringLiteral("PropertiesChanged"),
                  this,
                  SLOT(getSessionActive()));
    m_bus.connect(s_login1Service,
                  m_sessionPath,
                  s_dbusPropertiesInterface,
                  QStringLiteral("PropertiesChanged"),
                  this,
                  SLOT(getVirtualTerminal()));
}

void LogindIntegration::getSessionActive()
{
    if (!m_connected || m_sessionPath.isEmpty()) {
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                          m_sessionPath,
                                                          s_dbusPropertiesInterface,
                                                          QStringLiteral("Get"));
    message.setArguments(QVariantList({s_login1SessionInterface, QStringLiteral("Active")}));
    QDBusPendingReply<QVariant> reply = m_bus.asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this](QDBusPendingCallWatcher *self) {
            QDBusPendingReply<QVariant> reply = *self;
            self->deleteLater();
            if (!reply.isValid()) {
                qCDebug(KWIN_CORE) << "Failed to get Active Property of logind session:" << reply.error().message();
                return;
            }
            const bool active = reply.value().toBool();
            if (m_sessionActive != active) {
                m_sessionActive = active;
                emit sessionActiveChanged(m_sessionActive);
            }
        }
    );
}

void LogindIntegration::getVirtualTerminal()
{
    if (!m_connected || m_sessionPath.isEmpty()) {
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                          m_sessionPath,
                                                          s_dbusPropertiesInterface,
                                                          QStringLiteral("Get"));
    message.setArguments(QVariantList({s_login1SessionInterface, QStringLiteral("VTNr")}));
    QDBusPendingReply<QVariant> reply = m_bus.asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this](QDBusPendingCallWatcher *self) {
            QDBusPendingReply<QVariant> reply = *self;
            self->deleteLater();
            if (!reply.isValid()) {
                qCDebug(KWIN_CORE) << "Failed to get VTNr Property of logind session:" << reply.error().message();
                return;
            }
            const int vt = reply.value().toUInt();
            if (m_vt != (int)vt) {
                m_vt = vt;
                emit virtualTerminalChanged(m_vt);
            }
        }
    );
}

void LogindIntegration::takeControl()
{
    if (!m_connected || m_sessionPath.isEmpty() || m_sessionControl) {
        return;
    }
    static bool s_recursionCheck = false;
    if (s_recursionCheck) {
        return;
    }
    s_recursionCheck = true;

    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                          m_sessionPath,
                                                          s_login1SessionInterface,
                                                          QStringLiteral("TakeControl"));
    message.setArguments(QVariantList({QVariant(false)}));
    QDBusPendingReply<void> session = m_bus.asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(session, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this](QDBusPendingCallWatcher *self) {
            s_recursionCheck = false;
            QDBusPendingReply<void> reply = *self;
            self->deleteLater();
            if (!reply.isValid()) {
                qCDebug(KWIN_CORE) << "Failed to get session control" << reply.error().message();
                emit hasSessionControlChanged(false);
                return;
            }
            qCDebug(KWIN_CORE) << "Gained session control";
            m_sessionControl = true;
            emit hasSessionControlChanged(true);
            m_bus.connect(s_login1Service, m_sessionPath,
                          s_login1SessionInterface, QStringLiteral("PauseDevice"),
                          this, SLOT(pauseDevice(uint,uint,QString)));
        }
    );
}

void LogindIntegration::releaseControl()
{
    if (!m_connected || m_sessionPath.isEmpty() || !m_sessionControl) {
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                          m_sessionPath,
                                                          s_login1SessionInterface,
                                                          QStringLiteral("ReleaseControl"));
    m_bus.asyncCall(message);
    m_sessionControl = false;
    emit hasSessionControlChanged(false);
}

int LogindIntegration::takeDevice(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        qCDebug(KWIN_CORE) << "Could not stat the path";
        return -1;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                          m_sessionPath,
                                                          s_login1SessionInterface,
                                                          QStringLiteral("TakeDevice"));
    message.setArguments(QVariantList({QVariant(major(st.st_rdev)), QVariant(minor(st.st_rdev))}));
    // intended to be a blocking call
    QDBusMessage reply = m_bus.call(message);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qCDebug(KWIN_CORE) << "Could not take device" << path << ", cause: " << reply.errorMessage();
        return -1;
    }
    return dup(reply.arguments().first().value<QDBusUnixFileDescriptor>().fileDescriptor());
}

void LogindIntegration::releaseDevice(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) {
        qCDebug(KWIN_CORE) << "Could not stat the file descriptor";
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                          m_sessionPath,
                                                          s_login1SessionInterface,
                                                          QStringLiteral("ReleaseDevice"));
    message.setArguments(QVariantList({QVariant(major(st.st_rdev)), QVariant(minor(st.st_rdev))}));
    m_bus.asyncCall(message);
}

void LogindIntegration::pauseDevice(uint devMajor, uint devMinor, const QString &type)
{
    if (QString::compare(type, QStringLiteral("pause"), Qt::CaseInsensitive) == 0) {
        // unconditionally call complete
        QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service, m_sessionPath, s_login1SessionInterface, QStringLiteral("PauseDeviceComplete"));
        message.setArguments(QVariantList({QVariant(devMajor), QVariant(devMinor)}));
        m_bus.asyncCall(message);
    }
}

void LogindIntegration::getSeat()
{
    if (m_sessionPath.isEmpty()) {
        return;
    }
    qDBusRegisterMetaType<DBusLogindSeat>();
    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                          m_sessionPath,
                                                          s_dbusPropertiesInterface,
                                                          QStringLiteral("Get"));
    message.setArguments(QVariantList({s_login1SessionInterface, QStringLiteral("Seat")}));
    QDBusPendingReply<QVariant> reply = m_bus.asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this](QDBusPendingCallWatcher *self) {
            QDBusPendingReply<QVariant> reply = *self;
            self->deleteLater();
            if (!reply.isValid()) {
                qCDebug(KWIN_CORE) << "Failed to get Seat Property of logind session:" << reply.error().message();
                return;
            }
            DBusLogindSeat seat = qdbus_cast<DBusLogindSeat>(reply.value().value<QDBusArgument>());
            const QString seatPath = seat.path.path();
            qCDebug(KWIN_CORE) << "Logind seat:" << seat.name << "/" << seatPath;
            if (m_seatPath != seatPath) {
                m_seatPath = seatPath;
            }
        }
    );
}

void LogindIntegration::switchVirtualTerminal(quint32 vtNr)
{
    if (!m_connected || m_seatPath.isEmpty()) {
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service,
                                                          m_seatPath,
                                                          QStringLiteral("org.freedesktop.login1.Seat"),
                                                          QStringLiteral("SwitchTo"));
    message.setArguments(QVariantList{vtNr});
    m_bus.asyncCall(message);
}

} // namespace
