#include "Autostart.h"

namespace hydra
{
namespace
{
const juce::String kTaskName ("Hydra Mixer");
const juce::String kRunKey ("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\Hydra Mixer");

juce::File exe() { return juce::File::getSpecialLocation (juce::File::currentExecutableFile); }

juce::String runKeyCommand() { return "\"" + exe().getFullPathName() + "\" " + Autostart::kArgument; }

/** Runs schtasks.exe (no console window) and returns its exit code; output receives what it printed. */
int schtasks (const juce::StringArray& args, juce::String* output = nullptr)
{
    juce::StringArray cmd ("schtasks.exe");
    cmd.addArray (args);
    juce::ChildProcess p;
    if (! p.start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return -1;
    const auto text = p.readAllProcessOutput();
    if (! p.waitForProcessToFinish (10000)) { p.kill(); return -1; }
    if (output != nullptr) *output = text;
    return (int) p.getExitCode();
}

juce::String userId()
{
    const auto domain = juce::SystemStats::getEnvironmentVariable ("USERDOMAIN", {});
    const auto user = juce::SystemStats::getLogonName();
    return domain.isNotEmpty() ? domain + "\\" + user : user;
}

juce::String taskXml (int priority)
{
    // Logon of this user, no delay; never stopped (no time limit, also on battery); priority 1 = high class.
    auto esc = [] (const juce::String& s) { return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;"); };
    juce::String x;
    x << "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
      << "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
      << "  <RegistrationInfo><Description>Starts the Hydra Mixer audio engine when you log on.</Description></RegistrationInfo>\r\n"
      << "  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" << esc (userId()) << "</UserId></LogonTrigger></Triggers>\r\n"
      << "  <Principals><Principal id=\"Author\"><UserId>" << esc (userId()) << "</UserId>"
      << "<LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
      << "  <Settings>\r\n"
      << "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
      << "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
      << "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
      << "    <AllowHardTerminate>false</AllowHardTerminate>\r\n"
      << "    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
      << "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
      << "    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n"
      << "    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
      << "    <Enabled>true</Enabled>\r\n"
      << "    <Hidden>false</Hidden>\r\n"
      << "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
      << "    <WakeToRun>false</WakeToRun>\r\n"
      << "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
      << "    <Priority>" << priority << "</Priority>\r\n"
      << "  </Settings>\r\n"
      << "  <Actions Context=\"Author\"><Exec>"
      << "<Command>\"" << esc (exe().getFullPathName()) << "\"</Command>"
      << "<Arguments>" << Autostart::kArgument << "</Arguments>"
      << "<WorkingDirectory>" << esc (exe().getParentDirectory().getFullPathName()) << "</WorkingDirectory>"
      << "</Exec></Actions>\r\n"
      << "</Task>\r\n";
    return x;
}

bool createTask (juce::String& error)
{
    auto file = juce::File::createTempFile (".xml");
    for (const int priority : { 1, 4 })   // high; normal if the system refuses high
    {
        file.replaceWithText (taskXml (priority), true, true, "\r\n");   // schtasks wants UTF-16
        juce::String out;
        const int code = schtasks ({ "/Create", "/F", "/TN", kTaskName, "/XML", file.getFullPathName() }, &out);
        if (code == 0) { file.deleteFile(); return true; }
        error = out.trim();
    }
    file.deleteFile();
    return false;
}

bool taskExists (juce::String* xml = nullptr)
{
    return schtasks ({ "/Query", "/TN", kTaskName, "/XML" }, xml) == 0;
}
} // namespace

bool Autostart::isEnabled()
{
    return taskExists() || juce::WindowsRegistry::valueExists (kRunKey);
}

juce::String Autostart::setEnabled (bool enable)
{
    if (! enable)
    {
        if (taskExists()) schtasks ({ "/Delete", "/F", "/TN", kTaskName });
        juce::WindowsRegistry::deleteValue (kRunKey);
        if (isEnabled()) return "Could not remove the autostart entry.";
        logLine ("autostart off");
        return {};
    }

    juce::String error;
    if (createTask (error))
    {
        juce::WindowsRegistry::deleteValue (kRunKey);   // only one way of starting
        logLine ("autostart on (task scheduler)");
        return {};
    }
    // Fallback: Run key (starts a little later, priority is raised by the app itself)
    logLine ("autostart: task scheduler failed (" + error + "), using the Run key");
    if (juce::WindowsRegistry::setValue (kRunKey, runKeyCommand()))
        return {};
    return "Could not create the autostart entry." + (error.isNotEmpty() ? "\n" + error : juce::String());
}

void Autostart::refreshIfMoved()
{
    juce::String xml;
    if (taskExists (&xml))
    {
        if (! xml.containsIgnoreCase (exe().getFullPathName()))
        {
            juce::String error;
            createTask (error);
            logLine ("autostart: task updated to " + exe().getFullPathName());
        }
        return;
    }
    if (juce::WindowsRegistry::valueExists (kRunKey) && juce::WindowsRegistry::getValue (kRunKey) != runKeyCommand())
        juce::WindowsRegistry::setValue (kRunKey, runKeyCommand());
}
} // namespace hydra
