#include "EngineRunner.h"
#include "Paths.h"
#include <BinaryData.h>
#include <string>

using juce::String;
using juce::StringArray;

EngineRunner::EngineRunner() : juce::Thread ("AI Stem engine") {}

EngineRunner::~EngineRunner()
{
    cancel();
    stopThread (8000);
}

// --------------------------------------------------------------------------
juce::File EngineRunner::writeEngineScript()
{
    auto dir = stempaths::engineDir();
    dir.createDirectory();
    auto file = dir.getChildFile ("rvc_engine.py");

    juce::MemoryBlock wanted (BinaryData::rvc_engine_py, (size_t) BinaryData::rvc_engine_pySize);
    bool same = false;
    if (file.existsAsFile())
    {
        juce::MemoryBlock current;
        same = file.loadFileAsData (current) && current == wanted;
    }
    if (! same)
        file.replaceWithData (wanted.getData(), wanted.getSize());
    return file;
}

// --------------------------------------------------------------------------
bool EngineRunner::startProbe()
{
    if (isThreadRunning())
        return false;
    cancelled = false;
    task = Task::probe;
    startThread();
    return true;
}

bool EngineRunner::startSeparate (const juce::File& input, const juce::File& outDir,
                                  const SeparationOptions& options)
{
    if (isThreadRunning())
        return false;
    cancelled = false;
    jobInput = input;
    jobOutDir = outDir;
    jobOptions = options;
    task = Task::separate;
    startThread();
    return true;
}

bool EngineRunner::startSetup (bool cpuOnly)
{
    if (isThreadRunning())
        return false;
    cancelled = false;
    jobCpuOnly = cpuOnly;
    task = Task::setup;
    startThread();
    return true;
}

void EngineRunner::cancel()
{
    cancelled = true;
    const juce::ScopedLock sl (procLock);
    if (current != nullptr)
        current->kill();
}

EngineInfo EngineRunner::getInfo() const
{
    const juce::ScopedLock sl (infoLock);
    return bestInfo;
}

StringArray EngineRunner::getBaseCommand() const
{
    const juce::ScopedLock sl (infoLock);
    return baseCommand;
}

// --------------------------------------------------------------------------
void EngineRunner::run()
{
    switch (task.load())
    {
        case Task::probe:    doProbe();    break;
        case Task::separate: doSeparate(); break;
        case Task::setup:    doSetup();    break;
        case Task::none:     break;
    }
}

EngineInfo EngineRunner::probeCommand (const StringArray& command, const juce::File& script)
{
    EngineInfo info;
    info.command = command;

    StringArray args (command);
    args.add ("-u");
    args.add (script.getFullPathName());
    args.add ("probe");

    juce::ChildProcess process;
    if (! process.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return info;

    const auto output = process.readAllProcessOutput();
    for (const auto& line : StringArray::fromLines (output))
    {
        if (! line.startsWith ("PROBE "))
            continue;
        const auto v = juce::JSON::parse (line.fromFirstOccurrenceOf ("PROBE ", false, false));
        if (! v.isObject())
            continue;

        info.valid = true;
        info.pythonOk = (bool) v.getProperty ("python_ok", false);
        info.rvc = (bool) v.getProperty ("rvc", false);
        info.soundfile = (bool) v.getProperty ("soundfile", false);
        info.cuda = (bool) v.getProperty ("cuda", false);
        info.nvidiaPresent = (bool) v.getProperty ("nvidia_gpu_present", false);
        info.ffmpeg = (bool) v.getProperty ("ffmpeg", false);
        info.pythonVersion = v.getProperty ("python", {}).toString();
        info.torchVersion = v.getProperty ("torch", {}).toString();
        info.gpuName = v.getProperty ("gpu", {}).toString();
        break;
    }
    return info;
}

void EngineRunner::doProbe()
{
    post ([] (EngineRunner& r) { r.listeners.call (&Listener::engineStatus, "Buscando Python y RVC..."); });

    const auto script = writeEngineScript();

    std::vector<StringArray> candidates;
    auto addFile = [&] (const juce::File& f)
    {
        if (f != juce::File() && f.existsAsFile())
            candidates.push_back (StringArray (f.getFullPathName()));
    };
    addFile (stempaths::getCustomPython());
    addFile (stempaths::venvPython());
    candidates.push_back (StringArray ("python"));
   #if JUCE_WINDOWS
    candidates.push_back (StringArray ("py", "-3.10"));
    candidates.push_back (StringArray ("py", "-3"));
   #endif
    candidates.push_back (StringArray ("python3"));

    EngineInfo best;
    StringArray base;
    for (const auto& cmd : candidates)
    {
        if (threadShouldExit() || cancelled)
            return;
        auto info = probeCommand (cmd, script);
        if (info.score() > best.score())
            best = info;
        if (base.isEmpty() && info.valid && info.pythonOk)
            base = cmd;
    }

    {
        const juce::ScopedLock sl (infoLock);
        bestInfo = best;
        baseCommand = base;
    }
    probed = true;
    post ([best] (EngineRunner& r) { r.listeners.call (&Listener::engineProbed, best); });
}

// --------------------------------------------------------------------------
void EngineRunner::handleLine (const String& line, juce::var& result, String& error)
{
    if (line.isEmpty())
        return;

    logTail.add (line);
    while (logTail.size() > 15)
        logTail.remove (0);

    if (line.startsWith ("PROGRESS "))
    {
        const float p = juce::jlimit (0.0f, 1.0f, line.fromFirstOccurrenceOf (" ", false, false).getFloatValue() / 100.0f);
        post ([p] (EngineRunner& r) { r.listeners.call (&Listener::engineProgress, p); });
    }
    else if (line.startsWith ("STATUS "))
    {
        const auto text = line.fromFirstOccurrenceOf (" ", false, false);
        post ([text] (EngineRunner& r) { r.listeners.call (&Listener::engineStatus, text); });
    }
    else if (line.startsWith ("RESULT "))
    {
        result = juce::JSON::parse (line.fromFirstOccurrenceOf (" ", false, false));
    }
    else if (line.startsWith ("ERROR "))
    {
        error = line.fromFirstOccurrenceOf (" ", false, false);
    }
}

int EngineRunner::runStreaming (const StringArray& args, juce::var& result, String& error)
{
    logTail.clear();

    juce::ChildProcess process;
    if (! process.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = "No se pudo iniciar Python (" + args.joinIntoString (" ") + ").";
        return -1;
    }

    {
        const juce::ScopedLock sl (procLock);
        current = &process;
    }

    std::string pending;
    char buffer[4096];
    for (;;)
    {
        const int n = process.readProcessOutput (buffer, (int) sizeof (buffer));
        if (n > 0)
        {
            pending.append (buffer, (size_t) n);
            size_t nl;
            while ((nl = pending.find ('\n')) != std::string::npos)
            {
                handleLine (String::fromUTF8 (pending.data(), (int) nl).trimEnd(), result, error);
                pending.erase (0, nl + 1);
            }
        }
        else if (! process.isRunning())
        {
            break;
        }
        else
        {
            juce::Thread::sleep (20);
        }
    }
    if (! pending.empty())
        handleLine (String::fromUTF8 (pending.data(), (int) pending.size()).trimEnd(), result, error);

    process.waitForProcessToFinish (5000);
    const int code = (int) process.getExitCode();

    {
        const juce::ScopedLock sl (procLock);
        current = nullptr;
    }
    return code;
}

void EngineRunner::doSeparate()
{
    const auto info = getInfo();
    if (! info.ready())
    {
        post ([] (EngineRunner& r)
        {
            r.listeners.call (&Listener::engineFinished, Task::separate, false, juce::var(),
                              String ("El motor no esta instalado. Usa el menu \"Motor\" para instalarlo."));
        });
        return;
    }

    const auto script = writeEngineScript();
    jobOutDir.createDirectory();

    StringArray args (info.command);
    for (const String& a : std::initializer_list<String> {
             "-u", script.getFullPathName(), "convert", jobInput.getFullPathName(),
             "--out", jobOutDir.getFullPathName(),
             "--model", jobOptions.modelPath,
             "--pitch", String (jobOptions.pitch),
             "--f0", jobOptions.f0,
             "--index-rate", String (jobOptions.indexRate, 3),
             "--protect", String (jobOptions.protect, 3),
             "--filter-radius", String (jobOptions.filterRadius),
             "--rms-mix", String (jobOptions.rmsMix, 3),
             "--device", jobOptions.device })
        args.add (a);
    if (jobOptions.indexPath.isNotEmpty())
    {
        args.add ("--index");
        args.add (jobOptions.indexPath);
    }

    juce::var result;
    String error;
    const int code = runStreaming (args, result, error);
    const bool ok = (code == 0) && result.isObject() && ! cancelled;

    String message;
    if (! ok)
    {
        if (cancelled)
            message = "Cancelado.";
        else
        {
            message = error.isNotEmpty() ? error : "El motor termino con codigo " + String (code) + ".";
            if (logTail.size() > 0)
                message += "\n\n" + logTail.joinIntoString ("\n");
        }
    }
    post ([ok, result, message] (EngineRunner& r)
    {
        r.listeners.call (&Listener::engineFinished, Task::separate, ok, result, message);
    });
}

void EngineRunner::doSetup()
{
    const auto base = getBaseCommand();
    if (base.isEmpty())
    {
        post ([] (EngineRunner& r)
        {
            r.listeners.call (&Listener::engineFinished, Task::setup, false, juce::var(),
                              String ("No se encontro Python 3.10.\n"
                                      "Instala Python 3.10.11 desde python.org y marca \"Add Python to PATH\"."));
        });
        return;
    }

    const auto script = writeEngineScript();
    StringArray args (base);
    for (const String& a : std::initializer_list<String> {
             "-u", script.getFullPathName(), "setup", "--venv", stempaths::venvDir().getFullPathName() })
        args.add (a);
    if (jobCpuOnly)
        args.add ("--cpu-only");

    juce::var result;
    String error;
    const int code = runStreaming (args, result, error);
    const bool ok = (code == 0) && ! cancelled;

    String message;
    if (! ok)
    {
        message = cancelled ? String ("Cancelado.")
                            : (error.isNotEmpty() ? error : "La instalacion fallo (codigo " + String (code) + ").");
        if (! cancelled && logTail.size() > 0)
            message += "\n\n" + logTail.joinIntoString ("\n");
    }

    if (ok)
        doProbe(); // refresca el motor detectado

    post ([ok, result, message] (EngineRunner& r)
    {
        r.listeners.call (&Listener::engineFinished, Task::setup, ok, result, message);
    });
}
