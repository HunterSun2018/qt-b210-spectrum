#include "MainWindow.h"

#include <cmath>
#include <algorithm>

#include <QDoubleSpinBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include "SdrWorker.h"
#include "SpectrumWidget.h"
#include "WaterfallWidget.h"

namespace
{
QVector<float> shiftSpectrumToCenter(const QVector<float> &spectrum, int offsetBins, float fillValue)
{
    if (spectrum.isEmpty())
    {
        return spectrum;
    }

    QVector<float> shifted(spectrum.size(), fillValue);
    for (int index = 0; index < spectrum.size(); ++index)
    {
        const int sourceIndex = index + offsetBins;
        if (sourceIndex >= 0 && sourceIndex < spectrum.size())
        {
            shifted[index] = spectrum[sourceIndex];
        }
    }
    return shifted;
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_worker(std::make_unique<SdrWorker>())
{
    buildUi();

    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startStreaming);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopStreaming);
    connect(m_rateSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSpectrumAxes);
    connect(m_freqSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSpectrumAxes);
    connect(m_demodFreqSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSpectrumAxes);

    connect(m_worker.get(), &SdrWorker::spectrumReady, this, &MainWindow::handleSpectrum);
    connect(m_worker.get(), &SdrWorker::signalCenterFrequencyUpdated, this, &MainWindow::handleSignalCenterFrequency);
    connect(m_worker.get(), &SdrWorker::statusChanged, this, &MainWindow::handleStatus);
    connect(m_worker.get(), &SdrWorker::errorOccurred, this, &MainWindow::handleError);
    connect(m_spectrumWidget, &SpectrumWidget::demodFrequencySelected, this,
            [this](double frequencyHz)
            {
                const auto [bandwidthHz, tunedFrequencyHz] =
                    m_worker->calculateSignalBandwidthAndFreq(static_cast<std::size_t>(std::llround(frequencyHz)),
                                                              static_cast<std::size_t>(std::llround(m_rateSpin->value())));
                Q_UNUSED(bandwidthHz);
                m_demodFreqSpin->setValue(tunedFrequencyHz);
            });

    updateSpectrumAxes();
}

MainWindow::~MainWindow()
{
    m_worker->stopStreaming();
    m_worker->wait();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_freqSpin && event->type() == QEvent::Wheel)
    {
        auto *wheelEvent = static_cast<QWheelEvent *>(event);
        const QPoint angleDelta = wheelEvent->angleDelta();
        if (!angleDelta.isNull())
        {
            const double steps = static_cast<double>(angleDelta.y()) / 120.0;
            if (steps != 0.0)
            {
                m_freqSpin->setValue(m_freqSpin->value() + (steps * m_freqSpin->singleStep()));
                wheelEvent->accept();
                return true;
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::startStreaming()
{
    SdrWorker::Settings settings;
    settings.inputSource = m_sourceCombo->currentData().toInt() == 0
                               ? SdrWorker::InputSource::Usrp
                               : SdrWorker::InputSource::Simulator;
    settings.fftProcessorMode = m_processorCombo->currentData().toInt() == 0
                                  ? SdrWorker::FftProcessorMode::FftProcessor
                                  : SdrWorker::FftProcessorMode::IqFftwProcessor;
    settings.demodMode = static_cast<SdrWorker::DemodMode>(m_demodCombo->currentData().toInt());
    settings.deviceArgs = m_deviceEdit->text();
    settings.rxFrontend = m_rxFrontendCombo->currentText();
    settings.antenna = m_rxAntennaCombo->currentText();
    settings.sampleRate = m_rateSpin->value();
    settings.centerFreq = m_freqSpin->value();
    settings.demodCenterFreq = m_demodFreqSpin->value();
    settings.gain = m_gainSpin->value();
    settings.squelchDb = m_squelchSpin->value();
    settings.fftSize = static_cast<std::size_t>(m_fftSpin->currentData().toInt());
    
    settings.udp.enabled = true;
    settings.udp.remoteAddress = "192.168.1.50";
    settings.udp.remotePort = 10112;
    settings.udp.bindAddress = "0.0.0.0";
    settings.udp.bindPort = 0;
    settings.udp.majorVersion = 0;
    settings.udp.minorVersion = 0;    

    m_startButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_worker->startStreaming(settings);
}

void MainWindow::stopStreaming()
{
    m_worker->stopStreaming();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
}

void MainWindow::handleSpectrum(const QVector<float> &spectrum)
{
    m_lastSpectrum = spectrum;
    m_spectrumWidget->setSpectrum(spectrum);
    refreshTrackedSpectrum();
    m_waterfallWidget->addSpectrumLine(spectrum);
}

void MainWindow::handleSignalCenterFrequency(double centerFrequencyHz)
{
    m_trackedCenterFreqHz = centerFrequencyHz;
    refreshTrackedSpectrum();
}

void MainWindow::handleStatus(const QString &status)
{
    m_statusLabel->setText("Status: " + status);
    if (status == "Stopped" || status == "Error")
    {
        m_startButton->setEnabled(true);
        m_stopButton->setEnabled(false);
    }
}

void MainWindow::handleError(const QString &errorText)
{
    QMessageBox::critical(this, "B210 Streaming Error", errorText);
}

void MainWindow::updateSpectrumAxes()
{
    m_spectrumWidget->setFrequencySpan(m_freqSpin->value(), m_rateSpin->value());
    m_spectrumWidget->setDemodMarker(
        m_demodFreqSpin->value(),
        static_cast<SdrWorker::DemodMode>(m_demodCombo->currentData().toInt()) != SdrWorker::DemodMode::None);
    refreshTrackedSpectrum();
}

void MainWindow::refreshTrackedSpectrum()
{
    const double sampleRateHz = m_rateSpin->value();
    const double inputCenterFreqHz = m_freqSpin->value();
    const bool demodEnabled =
        static_cast<SdrWorker::DemodMode>(m_demodCombo->currentData().toInt()) != SdrWorker::DemodMode::None;

    const double trackedCenterFreqHz =
        std::isfinite(m_trackedCenterFreqHz) ? m_trackedCenterFreqHz : inputCenterFreqHz;
    m_trackedSpectrumWidget->setFrequencySpan(trackedCenterFreqHz, sampleRateHz);
    m_trackedSpectrumWidget->setDemodMarker(m_demodFreqSpin->value(), demodEnabled);

    if (m_lastSpectrum.isEmpty() || sampleRateHz <= 0.0)
    {
        m_trackedSpectrumWidget->setSpectrum(m_lastSpectrum);
        return;
    }

    const auto binCount = std::max<qsizetype>(1, m_lastSpectrum.size() - 1);
    const double binWidthHz = sampleRateHz / static_cast<double>(binCount);
    const int offsetBins = static_cast<int>(std::lround((trackedCenterFreqHz - inputCenterFreqHz) / binWidthHz));
    m_trackedSpectrumWidget->setSpectrum(shiftSpectrumToCenter(m_lastSpectrum, offsetBins, -120.0f));
}

void MainWindow::buildUi()
{
    setWindowTitle("B210 Spectrum Viewer");
    resize(1280, 860);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);

    auto *controlBox = new QGroupBox("Receiver Controls", central);
    auto *controlLayout = new QGridLayout(controlBox);

    m_sourceCombo = new QComboBox(controlBox);
    m_sourceCombo->addItem("USRP B210", 0);
    m_sourceCombo->addItem("Simulator", 1);

    m_rxFrontendCombo = new QComboBox(controlBox);
    m_rxFrontendCombo->addItem("A", 0);
    m_rxFrontendCombo->addItem("B", 1);
    connect(m_rxFrontendCombo, &QComboBox::currentIndexChanged, this,
            [this](int index)
            {
                const QString &text = m_rxFrontendCombo->itemText(index);
                m_worker->setRxFrontend(text);
            });

    m_rxAntennaCombo = new QComboBox(controlBox);
    m_rxAntennaCombo->addItem("TX/RX", 0);
    m_rxAntennaCombo->addItem("RX2", 1);
    connect(m_rxAntennaCombo, &QComboBox::currentIndexChanged, this,
            [this](int index)
            {
                const QString &antenna = m_rxAntennaCombo->itemText(index);
                m_worker->setRxAntenna(antenna);
            });

    m_processorCombo = new QComboBox(controlBox);
    m_processorCombo->addItem("FftProcessor", 0);
    m_processorCombo->addItem("IqFftwProcessor", 1);
    connect(m_processorCombo, &QComboBox::currentIndexChanged, this,
            [this](int index)
            {
                const auto mode = static_cast<SdrWorker::FftProcessorMode>(m_processorCombo->currentData().toInt());
                m_worker->setFftProcessor(mode);
            });

    m_demodCombo = new QComboBox(controlBox);
    m_demodCombo->addItem("Off", static_cast<int>(SdrWorker::DemodMode::None));
    m_demodCombo->addItem("FM", static_cast<int>(SdrWorker::DemodMode::FM));
    m_demodCombo->addItem("AM", static_cast<int>(SdrWorker::DemodMode::AM));
    connect(m_demodCombo, &QComboBox::currentIndexChanged, this,
            [this](int index)
            {
                Q_UNUSED(index);
                const auto mode = static_cast<SdrWorker::DemodMode>(m_demodCombo->currentData().toInt());
                m_worker->setDemodMode(mode);
                updateSpectrumAxes();
            });

    m_deviceEdit = new QLineEdit();
    m_deviceEdit->setPlaceholderText("type=b200 or leave empty for auto-discovery");

    m_rateSpin = new QDoubleSpinBox(controlBox);
    m_rateSpin->setRange(1.0e5, 6.0e7);
    m_rateSpin->setDecimals(0);
    m_rateSpin->setSingleStep(1.0e5);
    m_rateSpin->setValue(1.0e6);
    m_rateSpin->setSuffix(" S/s");
    connect(m_rateSpin, &QDoubleSpinBox::valueChanged, this,
            [this](double value)
            {
                m_worker->setRxSampleRate(value);
            });

    m_freqSpin = new QDoubleSpinBox(controlBox);
    m_freqSpin->setRange(50.0, 6.0e9);
    m_freqSpin->setDecimals(0);
    m_freqSpin->setSingleStep(1.0e4);
    m_freqSpin->setValue(103.9e6);
    m_freqSpin->setSuffix(" Hz");
    m_freqSpin->installEventFilter(this);
    connect(m_freqSpin, &QDoubleSpinBox::valueChanged, this,
            [this](double value)
            {
                m_worker->setFxCenterFreq(value);
            });

    m_demodFreqSpin = new QDoubleSpinBox(controlBox);
    m_demodFreqSpin->setRange(50.0, 6.0e9);
    m_demodFreqSpin->setDecimals(0);
    m_demodFreqSpin->setSingleStep(1.0e5);
    m_demodFreqSpin->setValue(103.9e6);
    m_demodFreqSpin->setSuffix(" Hz");
    connect(m_demodFreqSpin, &QDoubleSpinBox::valueChanged, this,
            [this](double value)
            {
                m_worker->setDemodCenterFreq(value);
            });

    m_gainSpin = new QDoubleSpinBox(controlBox);
    m_gainSpin->setRange(0.0, 76.0);
    m_gainSpin->setDecimals(1);
    m_gainSpin->setSingleStep(1.0);
    m_gainSpin->setValue(30.0);
    m_gainSpin->setSuffix(" dB");
    connect(m_gainSpin, &QDoubleSpinBox::valueChanged, this,
            [this](double value)
            {
                m_worker->setRxGain(value);
            });

    m_squelchSpin = new QDoubleSpinBox(controlBox);
    m_squelchSpin->setRange(-120.0, 0.0);
    m_squelchSpin->setDecimals(1);
    m_squelchSpin->setSingleStep(1.0);
    m_squelchSpin->setValue(-55.0);
    m_squelchSpin->setSuffix(" dBFS");

    m_fftSpin = new QComboBox(controlBox);
    m_fftSpin->addItem("1024", 1024);
    m_fftSpin->addItem("2048", 2048);
    m_fftSpin->addItem("4096", 4096);
    m_fftSpin->addItem("8192", 8192);
    m_fftSpin->setCurrentText("2048");
    connect(m_fftSpin, &QComboBox::currentIndexChanged, this,
            [this](int index)
            {
                const std::size_t fftSize = static_cast<std::size_t>(m_fftSpin->currentData().toInt());
                m_worker->setFftSize(fftSize);
            });

    m_startButton = new QPushButton("Start", controlBox);
    m_stopButton = new QPushButton("Stop", controlBox);
    m_stopButton->setEnabled(false);

    m_statusLabel = new QLabel("Status: Idle", controlBox);

    controlLayout->addWidget(new QLabel("Source :", controlBox), 0, 0);
    controlLayout->addWidget(m_sourceCombo, 0, 1);

    controlLayout->addWidget(new QLabel("RX Frontend :", controlBox), 0, 2);
    controlLayout->addWidget(m_rxFrontendCombo, 0, 3);
    controlLayout->addWidget(new QLabel("RX Antenna :", controlBox), 0, 4);
    controlLayout->addWidget(m_rxAntennaCombo, 0, 5);
    controlLayout->addWidget(new QLabel("Gain :", controlBox), 0, 6);
    controlLayout->addWidget(m_gainSpin, 0, 7);

    controlLayout->addWidget(new QLabel("Processor :", controlBox), 1, 0);
    controlLayout->addWidget(m_processorCombo, 1, 1);

    controlLayout->addWidget(new QLabel("FFT Size :", controlBox), 1, 2);
    controlLayout->addWidget(m_fftSpin, 1, 3);

    controlLayout->addWidget(new QLabel("Demod :", controlBox), 1, 4);
    controlLayout->addWidget(m_demodCombo, 1, 5);
     controlLayout->addWidget(new QLabel("Squelch :", controlBox), 1, 6);
    controlLayout->addWidget(m_squelchSpin, 1, 7);
    // controlLayout->addWidget(new QLabel("Device Args :", controlBox), 1, 2);
    // controlLayout->addWidget(m_deviceEdit, 1, 3);

    // Row 2 has the main tuning controls
    controlLayout->addWidget(new QLabel("Sample Rate :", controlBox), 2, 0);
    controlLayout->addWidget(m_rateSpin, 2, 1);
    controlLayout->addWidget(new QLabel("Center Freq :", controlBox), 2, 2);
    controlLayout->addWidget(m_freqSpin, 2, 3);
    controlLayout->addWidget(new QLabel("Demod Freq :", controlBox), 2, 4);
    controlLayout->addWidget(m_demodFreqSpin, 2, 5);
    
   

    // Row 3 has the status and start/stop buttons
    controlLayout->addWidget(m_statusLabel, 4, 0, 1, 2);
    controlLayout->addWidget(m_startButton, 4, 4);
    controlLayout->addWidget(m_stopButton, 4, 6);

    m_spectrumWidget = new SpectrumWidget(central);
    m_spectrumWidget->setTitle("Input Spectrum (dBm)");
    m_trackedSpectrumWidget = new SpectrumWidget(central);
    m_trackedSpectrumWidget->setTitle("Signal-Centered Spectrum (dBm)");
    m_waterfallWidget = new WaterfallWidget(central);

    layout->addWidget(controlBox);
    layout->addWidget(m_spectrumWidget, 1);
    layout->addWidget(m_trackedSpectrumWidget, 1);
    layout->addWidget(m_waterfallWidget, 1);

    setCentralWidget(central);
}
