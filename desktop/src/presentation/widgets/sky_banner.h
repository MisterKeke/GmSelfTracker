#pragma once

#include <QWidget>

namespace gmst {

class SkyBanner final : public QWidget {
public:
    explicit SkyBanner(QWidget *parent = nullptr);
    void setHeroMode(bool enabled);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool m_heroMode = false;
};

} // namespace gmst
