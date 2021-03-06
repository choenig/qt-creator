/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "pythonutils.h"

#include "pythonconstants.h"
#include "pythonproject.h"
#include "pythonrunconfiguration.h"
#include "pythonsettings.h"

#include <coreplugin/infobar.h>
#include <coreplugin/progressmanager/progressmanager.h>

#include <languageclient/languageclientsettings.h>
#include <languageclient/languageclientmanager.h>

#include <projectexplorer/session.h>
#include <projectexplorer/target.h>

#include <texteditor/textdocument.h>

#include <utils/qtcassert.h>
#include <utils/synchronousprocess.h>

#include <QDir>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QTimer>

using namespace Utils;

namespace Python {
namespace Internal {

static constexpr char startPylsInfoBarId[] = "Python::StartPyls";
static constexpr char installPylsInfoBarId[] = "Python::InstallPyls";
static constexpr char enablePylsInfoBarId[] = "Python::EnablePyls";
static constexpr char installPylsTaskId[] = "Python::InstallPylsTask";
static constexpr char pythonUtilsTrContext[] = "Python::Utils";

struct PythonLanguageServerState
{
    enum {
        CanNotBeInstalled,
        CanBeInstalled,
        AlreadyInstalled,
        AlreadyConfigured,
        ConfiguredButDisabled
    } state;
    FilePath pylsModulePath;
};

static QString pythonName(const FilePath &pythonPath)
{
    static QHash<FilePath, QString> nameForPython;
    if (!pythonPath.exists())
        return {};
    QString name = nameForPython.value(pythonPath);
    if (name.isEmpty()) {
        SynchronousProcess pythonProcess;
        pythonProcess.setTimeoutS(2);
        const CommandLine pythonVersionCommand(pythonPath, {"--version"});
        const SynchronousProcessResponse response = pythonProcess.runBlocking(pythonVersionCommand);
        if (response.result != SynchronousProcessResponse::Finished)
            return {};
        name = response.allOutput().trimmed();
        nameForPython[pythonPath] = name;
    }
    return name;
}

FilePath getPylsModulePath(CommandLine pylsCommand)
{
    pylsCommand.addArg("-h");
    SynchronousProcess pythonProcess;
    pythonProcess.setEnvironment(pythonProcess.environment() + QStringList("PYTHONVERBOSE=x"));
    SynchronousProcessResponse response = pythonProcess.runBlocking(pylsCommand);

    static const QString pylsInitPattern = "(.*)"
                                           + QRegularExpression::escape(
                                               QDir::toNativeSeparators("/pyls/__init__.py"))
                                           + '$';
    static const QRegularExpression regexCached(" matches " + pylsInitPattern,
                                                QRegularExpression::MultilineOption);
    static const QRegularExpression regexNotCached(" code object from " + pylsInitPattern,
                                                   QRegularExpression::MultilineOption);

    const QString &output = response.allOutput();
    for (auto regex : {regexCached, regexNotCached}) {
        QRegularExpressionMatch result = regex.match(output);
        if (result.hasMatch())
            return FilePath::fromUserInput(result.captured(1));
    }
    return {};
}

QList<const LanguageClient::StdIOSettings *> configuredPythonLanguageServer()
{
    using namespace LanguageClient;
    QList<const StdIOSettings *> result;
    for (const BaseSettings *setting : LanguageClientManager::currentSettings()) {
        if (setting->m_languageFilter.isSupported(FilePath::fromString("foo.py"),
                                                  Constants::C_PY_MIMETYPE)) {
            result << dynamic_cast<const StdIOSettings *>(setting);
        }
    }
    return result;
}

static PythonLanguageServerState checkPythonLanguageServer(const FilePath &python)
{
    using namespace LanguageClient;
    SynchronousProcess pythonProcess;
    const CommandLine pythonLShelpCommand(python, {"-m", "pyls", "-h"});
    SynchronousProcessResponse response = pythonProcess.runBlocking(pythonLShelpCommand);
    if (response.allOutput().contains("Python Language Server")) {
        const FilePath &modulePath = getPylsModulePath(pythonLShelpCommand);
        for (const StdIOSettings *serverSetting : configuredPythonLanguageServer()) {
            if (modulePath == getPylsModulePath(serverSetting->command())) {
                return {serverSetting->m_enabled ? PythonLanguageServerState::AlreadyConfigured
                                                 : PythonLanguageServerState::ConfiguredButDisabled,
                        FilePath()};
            }
        }

        return {PythonLanguageServerState::AlreadyInstalled, getPylsModulePath(pythonLShelpCommand)};
    }

    const CommandLine pythonPipVersionCommand(python, {"-m", "pip", "-V"});
    response = pythonProcess.runBlocking(pythonPipVersionCommand);
    if (response.allOutput().startsWith("pip "))
        return {PythonLanguageServerState::CanBeInstalled, FilePath()};
    else
        return {PythonLanguageServerState::CanNotBeInstalled, FilePath()};
}

FilePath detectPython(const FilePath &documentPath)
{
    FilePath python;

    PythonProject *project = qobject_cast<PythonProject *>(
        ProjectExplorer::SessionManager::projectForFile(documentPath));
    if (!project)
        project = qobject_cast<PythonProject *>(ProjectExplorer::SessionManager::startupProject());

    if (project) {
        if (auto target = project->activeTarget()) {
            if (auto runConfig = qobject_cast<PythonRunConfiguration *>(
                    target->activeRunConfiguration())) {
                python = FilePath::fromString(runConfig->interpreter());
            }
        }
    }

    if (!python.exists())
        python = PythonSettings::defaultInterpreter().command;

    if (!python.exists() && !PythonSettings::interpreters().isEmpty())
        python = PythonSettings::interpreters().first().command;

    return python;
}

const LanguageClient::StdIOSettings *languageServerForPython(const FilePath &python)
{
    return findOrDefault(configuredPythonLanguageServer(),
                         [pythonModulePath = getPylsModulePath(CommandLine(python, {"-m", "pyls"}))](
                             const LanguageClient::StdIOSettings *setting) {
                             return getPylsModulePath(setting->command()) == pythonModulePath;
                         });
}

static LanguageClient::Client *registerLanguageServer(const FilePath &python)
{
    auto *settings = new LanguageClient::StdIOSettings();
    settings->m_executable = python.toString();
    settings->m_arguments = "-m pyls";
    settings->m_name = QCoreApplication::translate(pythonUtilsTrContext,
                                                   "Python Language Server (%1)")
                           .arg(pythonName(python));
    settings->m_languageFilter.mimeTypes = QStringList(Constants::C_PY_MIMETYPE);
    LanguageClient::LanguageClientManager::registerClientSettings(settings);
    return LanguageClient::LanguageClientManager::clientForSetting(settings).value(0);
}

class PythonLSInstallHelper : public QObject
{
    Q_OBJECT
public:
    PythonLSInstallHelper(const FilePath &python, QPointer<TextEditor::TextDocument> document)
        : m_python(python)
        , m_document(document)
    {
        m_watcher.setFuture(m_future.future());
    }

    void run()
    {
        Core::ProgressManager::addTask(m_future.future(), "Install PyLS", installPylsTaskId);
        connect(&m_process,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                &PythonLSInstallHelper::installFinished);
        connect(&m_process,
                &QProcess::readyReadStandardError,
                this,
                &PythonLSInstallHelper::errorAvailable);
        connect(&m_process,
                &QProcess::readyReadStandardOutput,
                this,
                &PythonLSInstallHelper::outputAvailable);

        connect(&m_killTimer, &QTimer::timeout, this, &PythonLSInstallHelper::cancel);
        connect(&m_watcher, &QFutureWatcher<void>::canceled, this, &PythonLSInstallHelper::cancel);

        // on windows the pyls 0.28.3 crashes with pylint so just install the pyflakes linter
        const QString &pylsVersion = HostOsInfo::isWindowsHost()
                                         ? QString{"python-language-server[pyflakes]"}
                                         : QString{"python-language-server[all]"};

        m_process.start(m_python.toString(), {"-m", "pip", "install", pylsVersion});

        Core::MessageManager::write(tr("Running '%1 %2' to install python language server")
                                        .arg(m_process.program(), m_process.arguments().join(' ')));

        m_killTimer.setSingleShot(true);
        m_killTimer.start(5 /*minutes*/ * 60 * 1000);
    }

private:
    void cancel()
    {
        SynchronousProcess::stopProcess(m_process);
        Core::MessageManager::write(
            tr("The Python language server installation canceled by %1.")
                .arg(m_killTimer.isActive() ? tr("user") : tr("time out")));
    }

    void installFinished(int exitCode, QProcess::ExitStatus exitStatus)
    {
        m_future.reportFinished();
        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            if (LanguageClient::Client *client = registerLanguageServer(m_python))
                LanguageClient::LanguageClientManager::reOpenDocumentWithClient(m_document, client);
        } else {
            Core::MessageManager::write(
                tr("Installing the Python language server failed with exit code %1").arg(exitCode));
        }
        deleteLater();
    }

    void outputAvailable()
    {
        const QString &stdOut = QString::fromLocal8Bit(m_process.readAllStandardOutput().trimmed());
        if (!stdOut.isEmpty())
            Core::MessageManager::write(stdOut);
    }

    void errorAvailable()
    {
        const QString &stdErr = QString::fromLocal8Bit(m_process.readAllStandardError().trimmed());
        if (!stdErr.isEmpty())
            Core::MessageManager::write(stdErr);
    }

    QFutureInterface<void> m_future;
    QFutureWatcher<void> m_watcher;
    QProcess m_process;
    QTimer m_killTimer;
    const FilePath m_python;
    QPointer<TextEditor::TextDocument> m_document;
};

static void installPythonLanguageServer(const FilePath &python,
                                        QPointer<TextEditor::TextDocument> document)
{
    document->infoBar()->removeInfo(installPylsInfoBarId);

    auto install = new PythonLSInstallHelper(python, document);
    install->run();
}

static void setupPythonLanguageServer(const FilePath &python,
                                      QPointer<TextEditor::TextDocument> document)
{
    document->infoBar()->removeInfo(startPylsInfoBarId);
    if (LanguageClient::Client *client = registerLanguageServer(python))
        LanguageClient::LanguageClientManager::reOpenDocumentWithClient(document, client);
}

static void enablePythonLanguageServer(const FilePath &python,
                                       QPointer<TextEditor::TextDocument> document)
{
    using namespace LanguageClient;
    document->infoBar()->removeInfo(enablePylsInfoBarId);
    if (const StdIOSettings *setting = languageServerForPython(python)) {
        LanguageClientManager::enableClientSettings(setting->m_id);
        if (const StdIOSettings *setting = languageServerForPython(python)) {
            if (Client *client = LanguageClientManager::clientForSetting(setting).value(0))
                LanguageClientManager::reOpenDocumentWithClient(document, client);
        }
    }
}

void updateEditorInfoBar(const FilePath &python, TextEditor::TextDocument *document)
{
    const PythonLanguageServerState &lsState = checkPythonLanguageServer(python);

    if (lsState.state == PythonLanguageServerState::CanNotBeInstalled)
        return;
    if (lsState.state == PythonLanguageServerState::AlreadyConfigured) {
        if (const LanguageClient::StdIOSettings *setting = languageServerForPython(python)) {
            if (LanguageClient::Client *client
                = LanguageClient::LanguageClientManager::clientForSetting(setting).value(0)) {
                LanguageClient::LanguageClientManager::reOpenDocumentWithClient(document, client);
            }
        }
        return;
    }

    resetEditorInfoBar(document);
    Core::InfoBar *infoBar = document->infoBar();
    if (lsState.state == PythonLanguageServerState::CanBeInstalled
        && infoBar->canInfoBeAdded(installPylsInfoBarId)) {
        auto message
            =  QCoreApplication::translate(pythonUtilsTrContext,
                  "Install and set up Python language server (PyLS) for %1 (%2). "
                  "The language server provides Python specific completions and annotations.")
                  .arg(pythonName(python), python.toUserOutput());
        Core::InfoBarEntry info(installPylsInfoBarId,
                                message,
                                Core::InfoBarEntry::GlobalSuppression::Enabled);
        info.setCustomButtonInfo(QCoreApplication::translate(pythonUtilsTrContext, "Install"),
                                 [=]() { installPythonLanguageServer(python, document); });
        infoBar->addInfo(info);
    } else if (lsState.state == PythonLanguageServerState::AlreadyInstalled
               && infoBar->canInfoBeAdded(startPylsInfoBarId)) {
        auto message = QCoreApplication::translate(pythonUtilsTrContext,
                                                   "Found a Python language server for %1 (%2). "
                                                   "Should this one be set up for this document?")
                           .arg(pythonName(python), python.toUserOutput());
        Core::InfoBarEntry info(startPylsInfoBarId,
                                message,
                                Core::InfoBarEntry::GlobalSuppression::Enabled);
        info.setCustomButtonInfo(QCoreApplication::translate(pythonUtilsTrContext, "Setup"),
                                 [=]() { setupPythonLanguageServer(python, document); });
        infoBar->addInfo(info);
    } else if (lsState.state == PythonLanguageServerState::ConfiguredButDisabled
               && infoBar->canInfoBeAdded(enablePylsInfoBarId)) {
        auto message = QCoreApplication::translate(pythonUtilsTrContext,
                                                   "Enable Python language server for %1 (%2)?")
                           .arg(pythonName(python), python.toUserOutput());
        Core::InfoBarEntry info(enablePylsInfoBarId,
                                message,
                                Core::InfoBarEntry::GlobalSuppression::Enabled);
        info.setCustomButtonInfo(QCoreApplication::translate(pythonUtilsTrContext, "Enable"),
                                 [=]() { enablePythonLanguageServer(python, document); });
        infoBar->addInfo(info);
    }
}

void resetEditorInfoBar(TextEditor::TextDocument *document)
{
    Core::InfoBar *infoBar = document->infoBar();
    infoBar->removeInfo(installPylsInfoBarId);
    infoBar->removeInfo(startPylsInfoBarId);
    infoBar->removeInfo(enablePylsInfoBarId);
}

} // namespace Internal
} // namespace Python

#include "pythonutils.moc"

