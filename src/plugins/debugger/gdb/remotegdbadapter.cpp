/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "remotegdbadapter.h"

#include "debuggeractions.h"
#include "gdbengine.h"
#include "procinterrupt.h"

#include <utils/qtcassert.h>
#include <coreplugin/icore.h>

#include <QtCore/QFileInfo>
#include <QtGui/QMessageBox>

namespace Debugger {
namespace Internal {

#define STRINGIFY_INTERNAL(x) #x
#define STRINGIFY(x) STRINGIFY_INTERNAL(x)
#define CB(callback) \
    static_cast<GdbEngine::AdapterCallback>(&RemoteGdbAdapter::callback), \
    STRINGIFY(callback)

///////////////////////////////////////////////////////////////////////
//
// RemoteGdbAdapter
//
///////////////////////////////////////////////////////////////////////

RemoteGdbAdapter::RemoteGdbAdapter(GdbEngine *engine, QObject *parent)
    : AbstractGdbAdapter(engine, parent)
{
    QTC_ASSERT(state() == AdapterNotRunning, qDebug() << state());
    connect(&m_gdbProc, SIGNAL(error(QProcess::ProcessError)),
        this, SIGNAL(error(QProcess::ProcessError)));
    connect(&m_gdbProc, SIGNAL(readyReadStandardOutput()),
        this, SIGNAL(readyReadStandardOutput()));
    connect(&m_gdbProc, SIGNAL(readyReadStandardError()),
        this, SIGNAL(readyReadStandardError()));
    connect(&m_gdbProc, SIGNAL(started()),
        this, SLOT(handleGdbStarted()));
    connect(&m_gdbProc, SIGNAL(finished(int, QProcess::ExitStatus)),
        this, SLOT(handleGdbFinished(int, QProcess::ExitStatus)));

    connect(&m_uploadProc, SIGNAL(error(QProcess::ProcessError)),
        this, SLOT(uploadProcError(QProcess::ProcessError)));
    connect(&m_uploadProc, SIGNAL(readyReadStandardOutput()),
        this, SLOT(readUploadStandardOutput()));
    connect(&m_uploadProc, SIGNAL(readyReadStandardError()),
        this, SLOT(readUploadStandardError()));
}

void RemoteGdbAdapter::startAdapter()
{
    QTC_ASSERT(state() == AdapterNotRunning, qDebug() << state());
    setState(AdapterStarting);
    debugMessage(_("TRYING TO START ADAPTER"));

    QStringList gdbArgs;
    gdbArgs.prepend(_("mi"));
    gdbArgs.prepend(_("-i"));

    if (!m_engine->m_outputCollector.listen()) {
        emit adapterStartFailed(tr("Cannot set up communication with child process: %1")
                .arg(m_engine->m_outputCollector.errorString()));
        return;
    }
    gdbArgs.prepend(_("--tty=") + m_engine->m_outputCollector.serverName());

    if (!startParameters().workingDir.isEmpty())
        setWorkingDirectory(startParameters().workingDir);
    if (!startParameters().environment.isEmpty())
        setEnvironment(startParameters().environment);

    QString location = theDebuggerStringSetting(GdbLocation);

/*  
    // FIXME: make asynchroneouis
    // Start the remote server
    if (startParameters().serverStartScript.isEmpty()) {
        showStatusMessage(_("No server start script given. "
            "Assuming server runs already."));
    } else {
        if (!startParameters().workingDir.isEmpty())
            m_uploadProc.setWorkingDirectory(startParameters().workingDir);
        if (!startParameters().environment.isEmpty())
            m_uploadProc.setEnvironment(startParameters().environment);
        m_uploadProc.start(_("/bin/sh ") + startParameters().serverStartScript);
        m_uploadProc.waitForStarted();
    }
*/

    // Start the debugger
    m_gdbProc.start(location, gdbArgs);
}

void RemoteGdbAdapter::handleGdbStarted()
{
    QTC_ASSERT(state() == AdapterStarting, qDebug() << state());
    setState(AdapterStarted);
    emit adapterStarted();
}

void RemoteGdbAdapter::uploadProcError(QProcess::ProcessError error)
{
    QString msg;
    switch (error) {
        case QProcess::FailedToStart:
            msg = tr("The upload process failed to start. Either the "
                "invoked script '%1' is missing, or you may have insufficient "
                "permissions to invoke the program.")
                .arg(theDebuggerStringSetting(GdbLocation));
            break;
        case QProcess::Crashed:
            msg = tr("The upload process crashed some time after starting "
                "successfully.");
            break;
        case QProcess::Timedout:
            msg = tr("The last waitFor...() function timed out. "
                "The state of QProcess is unchanged, and you can try calling "
                "waitFor...() again.");
            break;
        case QProcess::WriteError:
            msg = tr("An error occurred when attempting to write "
                "to the upload process. For example, the process may not be running, "
                "or it may have closed its input channel.");
            break;
        case QProcess::ReadError:
            msg = tr("An error occurred when attempting to read from "
                "the upload process. For example, the process may not be running.");
            break;
        default:
            msg = tr("An unknown error in the upload process occurred. "
                "This is the default return value of error().");
    }

    m_engine->showStatusMessage(msg);
    QMessageBox::critical(m_engine->mainWindow(), tr("Error"), msg);
}

void RemoteGdbAdapter::readUploadStandardOutput()
{
    QByteArray ba = m_uploadProc.readAllStandardOutput();
    m_engine->gdbOutputAvailable(LogOutput, QString::fromLocal8Bit(ba, ba.length()));
}

void RemoteGdbAdapter::readUploadStandardError()
{
    QByteArray ba = m_uploadProc.readAllStandardError();
    m_engine->gdbOutputAvailable(LogError, QString::fromLocal8Bit(ba, ba.length()));
}

void RemoteGdbAdapter::prepareInferior()
{
    QTC_ASSERT(state() == AdapterStarted, qDebug() << state());
    setState(InferiorPreparing);
    if (!startParameters().processArgs.isEmpty())
        m_engine->postCommand(_("-exec-arguments ")
            + startParameters().processArgs.join(_(" ")));
    QFileInfo fi(m_engine->startParameters().executable);
    m_engine->postCommand(_("-file-exec-and-symbols \"%1\"").arg(fi.absoluteFilePath()),
        CB(handleFileExecAndSymbols));
}

void RemoteGdbAdapter::handleFileExecAndSymbols(const GdbResultRecord &response, const QVariant &)
{
    QTC_ASSERT(state() == InferiorPreparing, qDebug() << state());
    if (response.resultClass == GdbResultDone) {
        //m_breakHandler->clearBreakMarkers();
        setState(InferiorPrepared);
        emit inferiorPrepared();
    } else if (response.resultClass == GdbResultError) {
        QString msg = tr("Starting executable failed:\n") +
            __(response.data.findChild("msg").data());
        setState(InferiorPreparationFailed);
        emit inferiorPreparationFailed(msg);
    }
}

void RemoteGdbAdapter::startInferior()
{
    QTC_ASSERT(state() == InferiorPrepared, qDebug() << state());
    setState(InferiorStarting);
    m_engine->postCommand(_("-exec-run"), CB(handleExecRun));
}

void RemoteGdbAdapter::handleExecRun(const GdbResultRecord &response, const QVariant &)
{
    QTC_ASSERT(state() == InferiorStarting, qDebug() << state());
    if (response.resultClass == GdbResultRunning) {
        setState(InferiorStarted);
        emit inferiorStarted();
    } else {
        QTC_ASSERT(response.resultClass == GdbResultError, /**/);
        const QByteArray &msg = response.data.findChild("msg").data();
        //QTC_ASSERT(status() == DebuggerInferiorRunning, /**/);
        //interruptInferior();
        setState(InferiorStartFailed);
        emit inferiorStartFailed(msg);
    }
}

void RemoteGdbAdapter::interruptInferior()
{
    debugMessage(_("TRYING TO INTERUPT INFERIOR"));
    if (m_engine->startMode() == StartRemote) {
        m_engine->postCommand(_("-exec-interrupt"));
        return;
    }

    const qint64 attachedPID = m_engine->inferiorPid();
    if (attachedPID <= 0) {
        debugMessage(_("TRYING TO INTERRUPT INFERIOR BEFORE PID WAS OBTAINED"));
        return;
    }

    if (!interruptProcess(attachedPID))
        debugMessage(_("CANNOT INTERRUPT %1").arg(attachedPID));
}

void RemoteGdbAdapter::shutdown()
{
    if (state() == InferiorStarted) {
        setState(InferiorShuttingDown);
        m_engine->postCommand(_("kill"), CB(handleKill));
        return;
    }

    if (state() == InferiorShutDown) {
        setState(AdapterShuttingDown);
        m_engine->postCommand(_("-gdb-exit"), CB(handleExit));
        return;
    }

    // FIXME: handle other states, too.
    QTC_ASSERT(state() == AdapterNotRunning, qDebug() << state());
}

void RemoteGdbAdapter::handleKill(const GdbResultRecord &response, const QVariant &)
{
    if (response.resultClass == GdbResultDone) {
        setState(InferiorShutDown);
        emit inferiorShutDown();
        shutdown(); // re-iterate...
    } else if (response.resultClass == GdbResultError) {
        QString msg = tr("Inferior process could not be stopped:\n") +
            __(response.data.findChild("msg").data());
        setState(InferiorShutdownFailed);
        emit inferiorShutdownFailed(msg);
    }
}

void RemoteGdbAdapter::handleExit(const GdbResultRecord &response, const QVariant &)
{
    if (response.resultClass == GdbResultDone) {
        // don't set state here, this will be handled in handleGdbFinished()
    } else if (response.resultClass == GdbResultError) {
        QString msg = tr("Gdb process could not be stopped:\n") +
            __(response.data.findChild("msg").data());
        emit adapterShutdownFailed(msg);
    }
}

void RemoteGdbAdapter::handleGdbFinished(int, QProcess::ExitStatus)
{
    debugMessage(_("GDB PROESS FINISHED"));
    setState(AdapterNotRunning);
    emit adapterShutDown();
}

} // namespace Internal
} // namespace Debugger
