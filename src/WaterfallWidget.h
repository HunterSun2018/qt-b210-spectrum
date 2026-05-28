#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

class WaterfallWidget : public QWidget
{
    Q_OBJECT

public:
    explicit WaterfallWidget(QWidget *parent = nullptr);

public slots:
    void addSpectrumLine(const QVector<float> &spectrum);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void ensureImage();
    QRgb colorForValue(float value) const;

    QImage m_image;
    float m_minDb = -120.0f;
    float m_maxDb = 0.0f;
};
