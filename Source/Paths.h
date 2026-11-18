#pragma once

#include <juce_core/juce_core.h>

// Rutas y ajustes persistentes del plugin (fuera del proyecto del DAW).
namespace stempaths
{
inline juce::File dataRoot()
{
    juce::File root;
   #if JUCE_WINDOWS
    // LOCALAPPDATA (no Roaming): los modelos y stems pesan GB
    auto local = juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {});
    if (local.isNotEmpty())
        root = juce::File (local);
   #endif
    if (root == juce::File())
        root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    return root.getChildFile ("AIVoiceConverter");
}

inline juce::File engineDir() { return dataRoot().getChildFile ("engine"); }
inline juce::File cacheDir()  { return dataRoot().getChildFile ("cache"); }
inline juce::File venvDir()   { return dataRoot().getChildFile ("venv"); }
inline juce::File modelsDir() { return dataRoot().getChildFile ("models"); }     // un modelo RVC por subcarpeta
inline juce::File capturesDir() { return dataRoot().getChildFile ("captures"); } // audio grabado desde la pista

inline juce::File venvPython()
{
   #if JUCE_WINDOWS
    return venvDir().getChildFile ("Scripts").getChildFile ("python.exe");
   #else
    return venvDir().getChildFile ("bin").getChildFile ("python");
   #endif
}

inline juce::File customPythonFile() { return dataRoot().getChildFile ("python_path.txt"); }

inline juce::File getCustomPython()
{
    auto f = customPythonFile();
    if (! f.existsAsFile())
        return {};
    return juce::File (f.loadFileAsString().trim());
}

inline void setCustomPython (const juce::File& python)
{
    dataRoot().createDirectory();
    if (python == juce::File())
        customPythonFile().deleteFile();
    else
        customPythonFile().replaceWithText (python.getFullPathName());
}
} // namespace stempaths
