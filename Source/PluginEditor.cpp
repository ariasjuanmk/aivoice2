#include "PluginEditor.h"
#include "Paths.h"

namespace
{
const juce::Colour kBg      { 0xff14161c };
const juce::Colour kPanel   { 0xff1e212a };
const juce::Colour kText    { 0xffe6e8ee };
const juce::Colour kDim     { 0xff8a90a0 };
const juce::Colour kAccent  { 0xff5aa0f0 };
const juce::Colour kGood    { 0xff6fcf8a };
const juce::Colour kWarn    { 0xffe8b04a };
const juce::Colour kBad     { 0xffef6b6b };
const juce::Colour kVoice   { 0xffb281f1 };

const char* const kF0Values[]     = { "rmvpe", "harvest", "crepe", "pm" };
const char* const kF0Names[]      = { "rmvpe (recomendado)", "harvest (lento, estable)", "crepe", "pm (rapido)" };
const char* const kDeviceValues[] = { "auto", "cpu", "cuda" };
const char* const kDeviceNames[]  = { "Auto (CUDA, si no CPU)", "Solo CPU", "Forzar CUDA" };

template <size_t N>
void fillCombo (juce::ComboBox& box, const char* const (&names)[N])
{
    for (size_t i = 0; i < N; ++i)
        box.addItem (names[i], (int) i + 1);
}

template <size_t N>
juce::String valueOf (const juce::ComboBox& box, const char* const (&values)[N])
{
    const int idx = box.getSelectedId() - 1;
    return (idx >= 0 && (size_t) idx < N) ? values[idx] : values[0];
}

template <size_t N>
void selectValue (juce::ComboBox& box, const char* const (&values)[N], const juce::String& v)
{
    for (size_t i = 0; i < N; ++i)
        if (v == values[i])
        {
            box.setSelectedId ((int) i + 1, juce::dontSendNotification);
            return;
        }
    box.setSelectedId (1, juce::dontSendNotification);
}

void styleLabel (juce::Label& l, float size, juce::Colour c, bool bold = false)
{
    l.setFont (juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)));
    l.setColour (juce::Label::textColourId, c);
}

void styleCaption (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    styleLabel (l, 12.0f, kDim);
}

void styleSlider (juce::Slider& s, double lo, double hi, double step, double def, int decimals, const juce::String& suffix = {})
{
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
    s.setRange (lo, hi, step);
    s.setNumDecimalPlacesToDisplay (decimals);
    s.setTextValueSuffix (suffix);
    s.setDoubleClickReturnValue (true, def);
}

juce::String formatTime (double seconds)
{
    const int total = (int) seconds;
    return juce::String::formatted ("%d:%02d", total / 60, total % 60);
}
} // namespace

// --------------------------------------------------------------------------
// StemDragHandle
// --------------------------------------------------------------------------
void StemDragHandle::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (isMouseOver() ? kAccent.withAlpha (0.35f) : kPanel.brighter (0.1f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (kText);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (label, getLocalBounds(), juce::Justification::centred);
}

void StemDragHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() < 6 || ! getFiles)
        return;
    auto files = getFiles();
    files.removeEmptyStrings();
    if (files.isEmpty())
        return;
    juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, this);
}

// --------------------------------------------------------------------------
// Editor
// --------------------------------------------------------------------------
AIVoiceConverterEditor::AIVoiceConverterEditor (AIVoiceConverterProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      wetAtt (p.apvts, "wet", wet),
      dryAtt (p.apvts, "dry", dry),
      offsetAtt (p.apvts, "offset", offset)
{
    // OJO: setSize() va al final del constructor (resized() usa componentes que todavia no existen).
    title.setText ("AI Voice Converter (RVC)", juce::dontSendNotification);
    styleLabel (title, 20.0f, kText, true);
    addAndMakeVisible (title);

    styleLabel (engineLabel, 13.0f, kDim);
    addAndMakeVisible (engineLabel);

    styleLabel (dropLabel, 14.0f, kText);
    dropLabel.setJustificationType (juce::Justification::centred);
    dropLabel.setInterceptsMouseClicks (false, false); // el clic llega al editor (abre el selector)
    addAndMakeVisible (dropLabel);

    styleLabel (statusLabel, 13.0f, kDim);
    statusLabel.setJustificationType (juce::Justification::topLeft);
    statusLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (statusLabel);

    styleLabel (recLabel, 13.0f, kDim);
    addAndMakeVisible (recLabel);

    styleCaption (modelL, "Modelo de voz (RVC)");
    addAndMakeVisible (modelL);
    modelBox.setTextWhenNothingSelected ("(sin modelos: usa \"Agregar modelo...\")");
    modelBox.onChange = [this]
    {
        if (const auto* m = selectedModel())
            processor.setOption ("model", m->pth.getFullPathName());
    };
    addAndMakeVisible (modelBox);

    // ---- parametros de RVC (se guardan en el proyecto) ----
    styleCaption (pitchL, "Tono (semitonos)");
    styleCaption (indexL, "Parecido (index rate)");
    styleCaption (protectL, "Proteger consonantes");
    styleCaption (filterL, "Filtro de pitch");
    styleCaption (rmsL, "Volumen del modelo");
    styleCaption (f0L, "Detector de tono");
    styleCaption (deviceL, "Dispositivo");

    styleSlider (pitch, -24, 24, 1, 0, 0, " st");
    styleSlider (indexRate, 0.0, 1.0, 0.01, 0.5, 2);
    styleSlider (protect, 0.0, 0.5, 0.01, 0.33, 2);
    styleSlider (filterRadius, 0, 7, 1, 3, 0);
    styleSlider (rmsMix, 0.0, 1.0, 0.01, 1.0, 2);
    pitch.setTooltip ("+12 = una octava arriba. Para pasar de voz masculina a femenina suele ir +12; al reves, -12.");
    indexRate.setTooltip ("Cuanto se usa el archivo .index: mas alto = mas parecido al modelo, pero puede sonar mas artificial.");
    protect.setTooltip ("Protege consonantes y respiraciones. 0.5 = maxima proteccion, 0 = desactivada.");
    filterRadius.setTooltip ("Suavizado del tono. 3 o mas reduce la voz entrecortada.");
    rmsMix.setTooltip ("1 = volumen del modelo, 0 = copia el volumen de la voz original.");

    pitch.setValue (processor.getOption ("pitch", "0").getDoubleValue(), juce::dontSendNotification);
    indexRate.setValue (processor.getOption ("indexRate", "0.5").getDoubleValue(), juce::dontSendNotification);
    protect.setValue (processor.getOption ("protect", "0.33").getDoubleValue(), juce::dontSendNotification);
    filterRadius.setValue (processor.getOption ("filterRadius", "3").getDoubleValue(), juce::dontSendNotification);
    rmsMix.setValue (processor.getOption ("rmsMix", "1.0").getDoubleValue(), juce::dontSendNotification);
    pitch.onValueChange        = [this] { processor.setOption ("pitch", juce::String (pitch.getValue())); };
    indexRate.onValueChange    = [this] { processor.setOption ("indexRate", juce::String (indexRate.getValue())); };
    protect.onValueChange      = [this] { processor.setOption ("protect", juce::String (protect.getValue())); };
    filterRadius.onValueChange = [this] { processor.setOption ("filterRadius", juce::String (filterRadius.getValue())); };
    rmsMix.onValueChange       = [this] { processor.setOption ("rmsMix", juce::String (rmsMix.getValue())); };

    fillCombo (f0Box, kF0Names);
    fillCombo (deviceBox, kDeviceNames);
    selectValue (f0Box, kF0Values, processor.getOption ("f0", "rmvpe"));
    selectValue (deviceBox, kDeviceValues, processor.getOption ("device", "auto"));
    f0Box.onChange     = [this] { processor.setOption ("f0", valueOf (f0Box, kF0Values)); };
    deviceBox.onChange = [this] { processor.setOption ("device", valueOf (deviceBox, kDeviceValues)); };

    for (auto* c : std::initializer_list<juce::Component*> { &pitch, &indexRate, &protect, &filterRadius, &rmsMix,
                                                             &f0Box, &deviceBox, &pitchL, &indexL, &protectL,
                                                             &filterL, &rmsL, &f0L, &deviceL })
        addAndMakeVisible (c);

    // ---- botones ----
    chooseButton.onClick = [this] { chooseFile(); };
    pasteButton.onClick = [this] { pastePath(); };
    pasteButton.setTooltip ("Pega la ruta de un archivo copiada desde el Explorador "
                            "(Shift + clic derecho > Copiar como ruta de acceso)");
    recordButton.onClick = [this] { toggleRecording(); };
    recordButton.setTooltip ("Pone el plugin a grabar lo que entra por esta pista. Dale play en el DAW sobre la parte de voz "
                             "y despues toca \"Detener y convertir\".");
    addModelButton.onClick = [this] { addModel(); };
    folderButton.onClick = [this]
    {
        stempaths::modelsDir().createDirectory();
        stempaths::modelsDir().revealToUser();
    };
    convertButton.onClick = [this] { if (araMode) startAraConvert(); else startConvert(); };
    cancelButton.onClick = [this] { processor.engine.cancel(); };
    engineButton.onClick = [this] { showEngineMenu(); };
    for (auto* b : { &chooseButton, &pasteButton, &recordButton, &addModelButton, &folderButton,
                     &convertButton, &cancelButton, &engineButton })
        addAndMakeVisible (b);
    cancelButton.setVisible (false);
    progressBar.setVisible (false);
    addAndMakeVisible (progressBar);

    // ---- resultado ----
    styleLabel (resultName, 14.0f, kText, true);
    addAndMakeVisible (resultName);

    drag.label = "arrastrar";
    drag.getFiles = [this] { return juce::StringArray (processor.getExportFile (0).getFullPathName()); };
    addAndMakeVisible (drag);

    styleSlider (wet, -60.0, 12.0, 0.1, 0.0, 1, " dB");
    styleSlider (dry, -60.0, 12.0, 0.1, -60.0, 1, " dB");
    styleSlider (offset, -600.0, 600.0, 0.001, 0.0, 3, " s");
    wet.setTooltip ("Nivel de la voz convertida en la salida");
    dry.setTooltip ("Nivel de la voz original en la salida (-60 = silenciada, solo se oye la convertida)");
    offset.setTooltip ("Ajuste fino de la posicion de la voz convertida respecto al proyecto");
    styleCaption (wetL, "Voz convertida");
    styleCaption (dryL, "Voz original");
    styleCaption (offsetL, "Ajuste de posicion");
    for (auto* c : std::initializer_list<juce::Component*> { &wet, &dry, &offset, &wetL, &dryL, &offsetL })
        addAndMakeVisible (c);

    creditLabel.setText (juce::String (juce::CharPointer_UTF8 (
                             "Motor: RVC (rvc-python). Usa solo modelos de voz propios o con permiso de quien los grabo.")),
                         juce::dontSendNotification);
    styleLabel (creditLabel, 11.0f, kDim);
    creditLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (creditLabel);

    processor.addChangeListener (this);
    processor.engine.addListener (this);

    scanModels (processor.getOption ("model", {}));
    refreshEngineLabel();
    refreshResult();
    setInputFile (juce::File(), 0.0);

    if (processor.engine.isBusy())
    {
        if (processor.engine.currentTask() != EngineRunner::Task::probe)
            setBusy (true);
    }
    else if (! processor.engine.hasProbed())
    {
        processor.engine.startProbe(); // recien ahora (no al escanear plugins)
    }

    setSize (680, 650);
    startTimerHz (20);
}

AIVoiceConverterEditor::~AIVoiceConverterEditor()
{
    stopTimer();
    processor.engine.removeListener (this);
    processor.removeChangeListener (this);
}

// ---- utilidades ----
bool AIVoiceConverterEditor::isAudioFile (const juce::File& f)
{
    static const juce::StringArray exts { ".wav", ".mp3", ".flac", ".ogg", ".aif", ".aiff", ".m4a", ".wma", ".aac" };
    return exts.contains (f.getFileExtension().toLowerCase());
}

const AIVoiceConverterEditor::ModelEntry* AIVoiceConverterEditor::selectedModel() const
{
    const int idx = modelBox.getSelectedId() - 1;
    if (idx >= 0 && (size_t) idx < models.size())
        return &models[(size_t) idx];
    return nullptr;
}

SeparationOptions AIVoiceConverterEditor::currentOptions() const
{
    SeparationOptions o;
    if (const auto* m = selectedModel())
    {
        o.modelPath = m->pth.getFullPathName();
        o.indexPath = m->index.existsAsFile() ? m->index.getFullPathName() : juce::String();
    }
    o.pitch = (int) pitch.getValue();
    o.f0 = valueOf (f0Box, kF0Values);
    o.indexRate = indexRate.getValue();
    o.protect = protect.getValue();
    o.filterRadius = (int) filterRadius.getValue();
    o.rmsMix = rmsMix.getValue();
    o.device = valueOf (deviceBox, kDeviceValues);
    return o;
}

void AIVoiceConverterEditor::scanModels (const juce::String& selectPath)
{
    models.clear();
    modelBox.clear (juce::dontSendNotification);

    const auto root = stempaths::modelsDir();
    if (root.isDirectory())
    {
        auto files = root.findChildFiles (juce::File::findFiles, true, "*.pth");
        for (const auto& f : files)
        {
            ModelEntry e;
            e.name = f.getFileNameWithoutExtension();
            e.pth = f;
            auto idx = f.getParentDirectory().findChildFiles (juce::File::findFiles, false, "*.index");
            if (idx.size() > 0)
                e.index = idx[0];
            models.push_back (e);
        }
    }
    std::sort (models.begin(), models.end(),
               [] (const ModelEntry& a, const ModelEntry& b) { return a.name.compareIgnoreCase (b.name) < 0; });

    int selectId = 0;
    for (size_t i = 0; i < models.size(); ++i)
    {
        modelBox.addItem (models[i].name + (models[i].index.existsAsFile() ? "" : "  (sin .index)"), (int) i + 1);
        if (selectPath.isNotEmpty() && models[i].pth.getFullPathName() == selectPath)
            selectId = (int) i + 1;
    }
    if (selectId == 0 && ! models.empty())
        selectId = 1;
    if (selectId > 0)
    {
        modelBox.setSelectedId (selectId, juce::dontSendNotification);
        processor.setOption ("model", models[(size_t) (selectId - 1)].pth.getFullPathName());
    }
}

void AIVoiceConverterEditor::addModel()
{
    chooser = std::make_unique<juce::FileChooser> ("Elegir modelo RVC (.pth) y, si lo tenes, su .index", juce::File(),
                                                   "*.pth;*.index");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [safe = juce::Component::SafePointer<AIVoiceConverterEditor> (this)] (const juce::FileChooser& fc)
    {
        if (safe == nullptr)
            return;
        const auto results = fc.getResults();

        juce::Array<juce::File> pths, indexes;
        for (const auto& f : results)
        {
            if (f.hasFileExtension ("pth"))
                pths.add (f);
            else if (f.hasFileExtension ("index"))
                indexes.add (f);
        }
        if (pths.isEmpty())
        {
            if (results.size() > 0)
                safe->showStatus ("Falta el archivo .pth del modelo.", kWarn);
            return;
        }

        juce::String lastPath;
        bool missingIndex = false;
        for (const auto& pth : pths)
        {
            const auto dir = stempaths::modelsDir().getChildFile (juce::File::createLegalFileName (pth.getFileNameWithoutExtension()));
            dir.createDirectory();
            const auto dstPth = dir.getChildFile (pth.getFileName());
            if (! pth.copyFileTo (dstPth))
            {
                safe->showStatus ("No se pudo copiar el modelo a " + dir.getFullPathName(), kBad);
                return;
            }
            lastPath = dstPth.getFullPathName();

            // .index: el elegido junto al modelo, o uno de la misma carpeta que mencione el nombre del modelo
            juce::File index;
            if (pths.size() == 1 && indexes.size() > 0)
                index = indexes[0];
            else
            {
                const auto base = pth.getFileNameWithoutExtension();
                auto found = pth.getParentDirectory().findChildFiles (juce::File::findFiles, false, "*" + base + "*.index");
                if (found.size() > 0)
                    index = found[0];
            }
            if (index.existsAsFile())
                index.copyFileTo (dir.getChildFile (index.getFileName()));
            else
                missingIndex = true;
        }

        safe->scanModels (lastPath);
        if (missingIndex)
            safe->showStatus ("Modelo agregado SIN .index (funciona, pero el parecido es menor). Si lo tenes, copialo en la "
                              "carpeta del modelo (boton \"Carpeta\").", kWarn);
        else
            safe->showStatus ("Modelo agregado.", kGood);
    });
}

void AIVoiceConverterEditor::setInputFile (const juce::File& f, double startSeconds)
{
    inputFile = f;
    inputStart = startSeconds;
    if (f.existsAsFile())
        dropLabel.setText (f.getFileName() + (startSeconds > 0.0 ? "   (grabado desde " + formatTime (startSeconds) + ")" : juce::String()),
                           juce::dontSendNotification);
    else if (processor.hasStems())
        dropLabel.setText ("Voz convertida lista (" + processor.getSourceName()
                           + ")\nSolta otro audio para convertir de nuevo",
                           juce::dontSendNotification);
    else
        dropLabel.setText ("Solta aca el audio de voz (WAV, MP3, FLAC...)\no hace clic para elegirlo   -   o usa \"Grabar desde la pista\"",
                           juce::dontSendNotification);
    repaint();
}

void AIVoiceConverterEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Elegir audio de voz", juce::File(),
                                                   "*.wav;*.mp3;*.flac;*.ogg;*.aif;*.aiff;*.m4a;*.wma;*.aac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<AIVoiceConverterEditor> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && fc.getResult().existsAsFile())
                              {
                                  safe->setInputFile (fc.getResult(), 0.0);
                                  safe->showStatus ("Archivo cargado. Elegi el modelo y toca \"Convertir\".", kGood);
                              }
                          });
}

bool AIVoiceConverterEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    // Acepta cualquier archivo; si no es audio, filesDropped avisa en pantalla.
    return files.size() > 0;
}

void AIVoiceConverterEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    dragHover = true;
    repaint();
}

void AIVoiceConverterEditor::fileDragExit (const juce::StringArray&)
{
    dragHover = false;
    repaint();
}

void AIVoiceConverterEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    repaint();
    if (araMode)
    {
        showStatus ("En modo ARA se usa el audio de los clips de esta pista: no hace falta soltar archivos.", kWarn);
        return;
    }
    if (busy || processor.isRecording())
    {
        showStatus ("Espera a que termine el proceso actual.", kWarn);
        return;
    }
    for (const auto& f : files)
        if (isAudioFile (juce::File (f)))
        {
            setInputFile (juce::File (f), 0.0);
            showStatus ("Archivo cargado. Elegi el modelo y toca \"Convertir\".", kGood);
            return;
        }
    showStatus ("Formato no soportado. Usa WAV, MP3, FLAC, OGG, AIFF, M4A, WMA o AAC.", kBad);
}

void AIVoiceConverterEditor::mouseUp (const juce::MouseEvent& e)
{
    if (! araMode && ! busy && ! processor.isRecording() && e.mouseWasClicked() && dropArea.contains (e.getPosition()))
        chooseFile();
}

void AIVoiceConverterEditor::showStatus (const juce::String& text, juce::Colour colour)
{
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, colour);
}

void AIVoiceConverterEditor::pastePath()
{
    if (araMode || busy || processor.isRecording())
        return;
    const auto text = juce::SystemClipboard::getTextFromClipboard().trim().unquoted().trim();
    const juce::File f (text);
    if (text.isNotEmpty() && f.existsAsFile() && isAudioFile (f))
    {
        setInputFile (f, 0.0);
        showStatus ("Archivo cargado. Elegi el modelo y toca \"Convertir\".", kGood);
    }
    else
    {
        showStatus ("El portapapeles no tiene la ruta de un archivo de audio. En el Explorador: "
                    "Shift + clic derecho sobre el archivo > Copiar como ruta de acceso.", kWarn);
    }
}

// ---- estado ----
void AIVoiceConverterEditor::setBusy (bool b)
{
    busy = b;
    const bool free = ! b;
    convertButton.setEnabled (free);
    chooseButton.setEnabled (free);
    pasteButton.setEnabled (free);
    recordButton.setEnabled (free);
    addModelButton.setEnabled (free);
    engineButton.setEnabled (free);
    modelBox.setEnabled (free);
    f0Box.setEnabled (free);
    deviceBox.setEnabled (free);
    cancelButton.setVisible (b);
    progressBar.setVisible (b);
    if (b)
        progressValue = 0.0;
    resized();
}

void AIVoiceConverterEditor::refreshResult()
{
    const bool has = processor.hasStems() || araMode;
    resultName.setText (araMode ? juce::String ("VOZ CONVERTIDA  (modo ARA)")
                                : has ? "VOZ CONVERTIDA" + (processor.getDeviceText().isNotEmpty() ? "   -   " + processor.getDeviceText() : juce::String())
                                      : juce::String ("VOZ CONVERTIDA  (todavia no hay)"),
                        juce::dontSendNotification);
    resultName.setColour (juce::Label::textColourId, has ? kText : kDim);
    drag.setVisible (has && ! araMode);
    for (auto* c : std::initializer_list<juce::Component*> { &wet, &dry, &offset })
        c->setEnabled (has);
}

void AIVoiceConverterEditor::refreshEngineLabel()
{
    const auto& eng = processor.engine;
    const auto info = eng.getInfo();
    juce::String text;
    juce::Colour col = kDim;

    if (! eng.hasProbed())
    {
        text = "Buscando motor (Python 3.10 + RVC)...";
    }
    else if (info.ready() && info.cuda)
    {
        text = "Motor listo - GPU: " + (info.gpuName.isNotEmpty() ? info.gpuName : juce::String ("NVIDIA")) + " (CUDA)";
        col = kGood;
    }
    else if (info.ready() && info.nvidiaPresent)
    {
        text = "Motor listo en CPU. Hay GPU NVIDIA pero PyTorch no tiene CUDA: Motor > Instalar motor.";
        col = kWarn;
    }
    else if (info.ready())
    {
        text = "Motor listo - CPU (no se detecto CUDA)";
        col = kGood;
    }
    else if (info.valid)
    {
        text = "Python " + info.pythonVersion + " encontrado, pero falta RVC: Motor > Instalar motor.";
        col = kWarn;
    }
    else
    {
        text = "No se encontro Python 3.10. Instala 3.10.11 desde python.org (Add to PATH) o usa Motor > Instalar motor.";
        col = kBad;
    }
    engineLabel.setText (text, juce::dontSendNotification);
    engineLabel.setColour (juce::Label::textColourId, col);
}

bool AIVoiceConverterEditor::engineReadyOrExplain()
{
    if (! processor.engine.getInfo().ready())
    {
        showStatus ("El motor no esta listo. Usa el menu \"Motor\" > Instalar motor.", kBad);
        return false;
    }
    if (selectedModel() == nullptr)
    {
        showStatus ("Primero agrega y elegi un modelo de voz (.pth).", kBad);
        return false;
    }
    return true;
}

void AIVoiceConverterEditor::toggleRecording()
{
    if (busy)
        return;

    if (! processor.isRecording())
    {
        if (! engineReadyOrExplain())
            return;
        const auto err = processor.startRecording();
        if (err.isNotEmpty())
        {
            showStatus (err, kBad);
            return;
        }
        recordButton.setButtonText ("Detener y convertir");
        for (auto* c : std::initializer_list<juce::Component*> { &chooseButton, &pasteButton, &convertButton, &addModelButton, &engineButton })
            c->setEnabled (false);
        showStatus ("Grabando. Dale play en el DAW sobre la parte de voz de esta pista y, al terminar, toca \"Detener y convertir\".", kWarn);
        return;
    }

    double start = 0.0;
    const auto file = processor.stopRecording (start);
    recordButton.setButtonText ("Grabar desde la pista");
    for (auto* c : std::initializer_list<juce::Component*> { &chooseButton, &pasteButton, &convertButton, &addModelButton, &engineButton })
        c->setEnabled (true);
    recLabel.setText ({}, juce::dontSendNotification);

    if (! file.existsAsFile())
    {
        showStatus ("No se grabo nada. Reproduce el proyecto en el DAW mientras esta grabando "
                    "(la pista debe tener el audio de voz antes de este plugin).", kBad);
        return;
    }
    setInputFile (file, start);
    startConvert();
}

void AIVoiceConverterEditor::startConvert()
{
    if (busy || processor.isRecording())
        return;
    if (! inputFile.existsAsFile())
    {
        showStatus ("Primero solta, elegi o graba un audio de voz.", kBad);
        return;
    }
    if (! engineReadyOrExplain())
        return;

    const auto err = processor.startSeparation (inputFile, currentOptions(), inputStart);
    if (err.isNotEmpty())
    {
        showStatus (err, kBad);
        return;
    }

    if (processor.engine.isBusy())
    {
        setBusy (true);
        showStatus ("Iniciando...", kDim);
    }
    else
    {
        showStatus ("Listo (resultado ya procesado, tomado de la cache).", kGood);
    }
}

void AIVoiceConverterEditor::startAraConvert()
{
    if (busy)
        return;

    const auto err = processor.startAraConversion (currentOptions());
    if (err.isNotEmpty())
    {
        showStatus (err, kBad);
        return;
    }

    if (processor.isAraBusy())
    {
        setBusy (true);
        showStatus ("Iniciando...", kDim);
    }
    else if (processor.getAraError().isNotEmpty())
    {
        showStatus (processor.getAraError(), kBad);
    }
    else
    {
        showStatus ("Listo (resultado ya procesado, tomado de la cache). Dale play en el DAW.", kGood);
    }
}

void AIVoiceConverterEditor::updateMode()
{
    const bool ara = processor.isAraActive();
    if (ara != araMode)
    {
        araMode = ara;
        for (auto* c : std::initializer_list<juce::Component*> { &chooseButton, &pasteButton, &recordButton })
            c->setVisible (! araMode);
        convertButton.setButtonText (araMode ? "Convertir clips de la pista" : "Convertir");
        refreshResult();
        resized();
        repaint();
    }

    if (araMode)
    {
        dropLabel.setText (processor.getAraStatusText(), juce::dontSendNotification);
        recLabel.setText ("Modo ARA: se usa el audio de los clips de esta pista (no hace falta grabar).",
                          juce::dontSendNotification);
    }
}

void AIVoiceConverterEditor::showEngineMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Detectar de nuevo");
    m.addSeparator();
    m.addItem (2, "Instalar motor (CUDA si hay GPU NVIDIA)");
    m.addItem (3, "Instalar motor solo CPU");
    m.addSeparator();
    m.addItem (4, "Elegir python.exe (3.10)...");
    m.addItem (5, "Volver a deteccion automatica");
    m.addItem (6, "Abrir carpeta de datos");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&engineButton),
                     [safe = juce::Component::SafePointer<AIVoiceConverterEditor> (this)] (int r)
    {
        if (safe == nullptr || r == 0)
            return;
        auto& self = *safe;
        auto& eng = self.processor.engine;

        auto confirmInstall = [safe] (bool cpuOnly)
        {
            auto opts = juce::MessageBoxOptions::makeOptionsOkCancel (
                juce::MessageBoxIconType::QuestionIcon, "Instalar motor",
                "Se crea un entorno de Python 3.10 propio en:\n" + stempaths::venvDir().getFullPathName()
                    + "\n\nInstala PyTorch 2.1.1" + (cpuOnly ? " (CPU)" : " (CUDA 11.8 si hay GPU NVIDIA)")
                    + ", RVC (rvc-python) y ffmpeg. Son varios GB y puede tardar varios minutos. Seguir?",
                "Instalar", "Cancelar");
            juce::AlertWindow::showAsync (opts, [safe, cpuOnly] (int result)
            {
                if (result != 1 || safe == nullptr)
                    return;
                if (safe->processor.engine.startSetup (cpuOnly))
                {
                    safe->setBusy (true);
                    safe->showStatus ("Instalando motor...", kDim);
                }
            });
        };

        switch (r)
        {
            case 1:
                if (eng.startProbe())
                    self.refreshEngineLabel();
                break;
            case 2: confirmInstall (false); break;
            case 3: confirmInstall (true); break;
            case 4:
                self.chooser = std::make_unique<juce::FileChooser> ("Elegir python.exe", juce::File(), "python*");
                self.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                           [safe] (const juce::FileChooser& fc)
                {
                    if (safe == nullptr || ! fc.getResult().existsAsFile())
                        return;
                    stempaths::setCustomPython (fc.getResult());
                    if (safe->processor.engine.startProbe())
                        safe->refreshEngineLabel();
                });
                break;
            case 5:
                stempaths::setCustomPython (juce::File());
                if (eng.startProbe())
                    self.refreshEngineLabel();
                break;
            case 6:
                stempaths::dataRoot().createDirectory();
                stempaths::dataRoot().revealToUser();
                break;
            default: break;
        }
    });
}

// ---- callbacks del motor ----
void AIVoiceConverterEditor::engineStatus (const juce::String& s)
{
    showStatus (s, kDim);
}

void AIVoiceConverterEditor::engineProgress (float p)
{
    progressValue = (double) p;
}

void AIVoiceConverterEditor::engineProbed (const EngineInfo&)
{
    refreshEngineLabel();
    if (! busy)
        statusLabel.setText ({}, juce::dontSendNotification);
}

void AIVoiceConverterEditor::engineFinished (EngineRunner::Task task, bool ok, const juce::var&,
                                             const juce::String& error)
{
    setBusy (false);
    refreshEngineLabel();

    if (araMode && task == EngineRunner::Task::separate)
    {
        if (processor.isAraBusy())
        {
            setBusy (true);   // quedan clips por convertir
            return;
        }
        if (! ok)
            showStatus (error, kBad);
        else if (processor.getAraError().isNotEmpty())
            showStatus (processor.getAraError(), kBad);
        else
            showStatus ("Listo. Dale play en el DAW: se oye la voz convertida (ajusta el mix abajo).", kGood);
        return;
    }

    if (ok)
        showStatus (task == EngineRunner::Task::setup ? "Motor instalado. Ya podes convertir."
                                                      : "Listo. Reproduci el proyecto: se oye la voz convertida (ajusta el mix abajo).",
                    kGood);
    else
        showStatus (error, kBad);
}

void AIVoiceConverterEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateMode();
    refreshResult();
    if (! araMode)
        setInputFile (inputFile, inputStart);
}

void AIVoiceConverterEditor::timerCallback()
{
    updateMode();
    if (processor.isRecording())
        recLabel.setText ("REC  " + formatTime (processor.getRecordedSeconds()), juce::dontSendNotification);
    repaint (resultArea);
}

// ---- dibujo y layout ----
void AIVoiceConverterEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    g.setColour (kPanel);
    const auto zone = dropArea.toFloat();
    g.fillRoundedRectangle (zone, 10.0f);
    if (dragHover)
    {
        g.setColour (kAccent.withAlpha (0.25f));
        g.fillRoundedRectangle (zone, 10.0f);
    }
    g.setColour (kAccent.withAlpha (dragHover ? 1.0f : 0.5f));
    g.drawRoundedRectangle (zone.reduced (1.0f), 10.0f, dragHover ? 2.5f : 1.5f);

    // panel del resultado con medidor
    const auto panel = resultArea.toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (panel, 8.0f);
    g.setColour ((processor.hasStems() || araMode) ? kVoice : kDim.withAlpha (0.4f));
    g.fillRoundedRectangle (panel.getX() + 6, panel.getY() + 8, 5.0f, 24.0f, 2.0f);

    if (processor.hasStems() || araMode)
    {
        auto meter = juce::Rectangle<float> (panel.getRight() - 214.0f, panel.getY() + 16.0f, 70.0f, 8.0f);
        g.setColour (kBg);
        g.fillRoundedRectangle (meter, 3.0f);
        const float lvl = juce::jlimit (0.0f, 1.0f, processor.getLevel (0));
        g.setColour (kVoice);
        g.fillRoundedRectangle (meter.withWidth (meter.getWidth() * lvl), 3.0f);
    }
}

void AIVoiceConverterEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (34);
    engineButton.setBounds (top.removeFromRight (100).reduced (0, 4));
    title.setBounds (top);

    engineLabel.setBounds (r.removeFromTop (22));
    r.removeFromTop (6);

    auto drop = r.removeFromTop (76);
    dropArea = drop;
    if (! araMode)
    {
        auto btns = drop.removeFromRight (150).reduced (8, 5);
        chooseButton.setBounds (btns.removeFromTop (30));
        btns.removeFromTop (4);
        pasteButton.setBounds (btns.removeFromTop (30));
    }
    dropLabel.setBounds (drop);
    r.removeFromTop (8);

    auto rec = r.removeFromTop (32);
    if (! araMode)
    {
        recordButton.setBounds (rec.removeFromLeft (190));
        rec.removeFromLeft (10);
    }
    recLabel.setBounds (rec);
    r.removeFromTop (8);

    modelL.setBounds (r.removeFromTop (18));
    auto mrow = r.removeFromTop (28);
    folderButton.setBounds (mrow.removeFromRight (80));
    mrow.removeFromRight (6);
    addModelButton.setBounds (mrow.removeFromRight (130));
    mrow.removeFromRight (6);
    modelBox.setBounds (mrow);
    r.removeFromTop (8);

    // parametros en dos columnas
    auto row = [&] () { return r.removeFromTop (28); };
    auto sliderRow = [&] (juce::Rectangle<int> area, juce::Label& l, juce::Component& c)
    {
        l.setBounds (area.removeFromLeft (128));
        c.setBounds (area.reduced (0, 2));
    };
    {
        auto a = row();
        auto left = a.removeFromLeft (a.getWidth() / 2).withTrimmedRight (8);
        sliderRow (left, pitchL, pitch);
        sliderRow (a.withTrimmedLeft (8), filterL, filterRadius);
    }
    {
        auto a = row();
        auto left = a.removeFromLeft (a.getWidth() / 2).withTrimmedRight (8);
        sliderRow (left, indexL, indexRate);
        sliderRow (a.withTrimmedLeft (8), rmsL, rmsMix);
    }
    {
        auto a = row();
        auto left = a.removeFromLeft (a.getWidth() / 2).withTrimmedRight (8);
        sliderRow (left, protectL, protect);
        sliderRow (a.withTrimmedLeft (8), f0L, f0Box);
    }
    {
        auto a = row();
        auto left = a.removeFromLeft (a.getWidth() / 2).withTrimmedRight (8);
        sliderRow (left, deviceL, deviceBox);
    }
    r.removeFromTop (6);

    // acciones
    auto actions = r.removeFromTop (34);
    convertButton.setBounds (actions.removeFromLeft (araMode ? 240 : 130));
    actions.removeFromLeft (8);
    if (busy)
    {
        cancelButton.setBounds (actions.removeFromLeft (100));
        actions.removeFromLeft (10);
        progressBar.setBounds (actions.reduced (0, 6));
    }
    r.removeFromTop (4);
    statusLabel.setBounds (r.removeFromTop (52));
    r.removeFromTop (4);

    // resultado
    auto head = r.removeFromTop (40);
    resultArea = head;
    auto inner = head.reduced (8, 6);
    drag.setBounds (inner.removeFromRight (84).reduced (0, 2));
    inner.removeFromRight (84);                  // medidor (se pinta)
    resultName.setBounds (inner.withTrimmedLeft (12));
    r.removeFromTop (4);

    auto w = row();
    sliderRow (w, wetL, wet);
    auto d = row();
    sliderRow (d, dryL, dry);
    auto o = row();
    sliderRow (o, offsetL, offset);
    r.removeFromTop (4);
    creditLabel.setBounds (r.removeFromTop (18));
}
