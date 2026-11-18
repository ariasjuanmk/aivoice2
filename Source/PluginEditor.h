#pragma once

#include "PluginProcessor.h"

// Asa para arrastrar un WAV desde el plugin hasta la linea de tiempo del DAW.
class StemDragHandle : public juce::Component
{
public:
    std::function<juce::StringArray()> getFiles;
    juce::String label = "arrastrar";

    void paint (juce::Graphics& g) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseEnter (const juce::MouseEvent&) override { setMouseCursor (juce::MouseCursor::DraggingHandCursor); }
};

class AIVoiceConverterEditor : public juce::AudioProcessorEditor,
                               public juce::FileDragAndDropTarget,
                               private juce::Timer,
                               private juce::ChangeListener,
                               private EngineRunner::Listener
{
public:
    explicit AIVoiceConverterEditor (AIVoiceConverterProcessor&);
    ~AIVoiceConverterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct ModelEntry
    {
        juce::String name;
        juce::File pth, index;
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void engineStatus (const juce::String&) override;
    void engineProgress (float) override;
    void engineProbed (const EngineInfo&) override;
    void engineFinished (EngineRunner::Task, bool, const juce::var&, const juce::String&) override;

    void setInputFile (const juce::File&, double startSeconds);
    void pastePath();
    void showStatus (const juce::String& text, juce::Colour colour);
    void chooseFile();
    void startConvert();
    void startAraConvert();
    void updateMode();
    void toggleRecording();
    void showEngineMenu();
    void refreshEngineLabel();
    void refreshResult();
    void setBusy (bool busy);
    void scanModels (const juce::String& selectPath);
    void addModel();
    const ModelEntry* selectedModel() const;
    SeparationOptions currentOptions() const;
    bool engineReadyOrExplain();
    static bool isAudioFile (const juce::File&);

    AIVoiceConverterProcessor& processor;

    juce::File inputFile;
    double inputStart = 0.0;
    bool busy = false;
    bool araMode = false;
    bool dragHover = false;
    juce::Rectangle<int> dropArea, resultArea;
    double progressValue = 0.0;

    std::vector<ModelEntry> models;

    juce::Label title, engineLabel, statusLabel, dropLabel, recLabel, modelL, resultName, creditLabel;
    juce::TextButton chooseButton { "Elegir archivo..." }, pasteButton { "Pegar ruta" },
                     recordButton { "Grabar desde la pista" },
                     addModelButton { "Agregar modelo..." }, folderButton { "Carpeta" },
                     convertButton { "Convertir" }, cancelButton { "Cancelar" },
                     engineButton { "Motor  v" };
    juce::ComboBox modelBox, f0Box, deviceBox;
    juce::Label f0L, deviceL, pitchL, indexL, protectL, filterL, rmsL, wetL, dryL, offsetL;
    juce::Slider pitch, indexRate, protect, filterRadius, rmsMix;
    juce::Slider wet, dry, offset;
    juce::ProgressBar progressBar { progressValue };
    StemDragHandle drag;

    juce::AudioProcessorValueTreeState::SliderAttachment wetAtt, dryAtt, offsetAtt;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AIVoiceConverterEditor)
};
