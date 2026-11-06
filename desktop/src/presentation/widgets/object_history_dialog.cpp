#include "presentation/widgets/object_history_dialog.h"

#include "infrastructure/database/game_repository.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>

namespace gmst {
namespace {

const std::array<std::pair<QString, QString>, 5> eventTypes{{
    {QStringLiteral("created"), QStringLiteral("Created")},
    {QStringLiteral("updated"), QStringLiteral("Updated")},
    {QStringLiteral("archived"), QStringLiteral("Archived")},
    {QStringLiteral("restored"), QStringLiteral("Restored")},
    {QStringLiteral("field_type_migrated"), QStringLiteral("Field type migrated")}
}};

QString valueLabel(const QJsonValue &value)
{
    if (value.isUndefined()) return QStringLiteral("—");
    if (value.isString()) return value.toString().isEmpty() ? QStringLiteral("(empty text)") : value.toString();
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}

class HistoryChart final : public QWidget {
public:
    explicit HistoryChart(QWidget *parent) : QWidget(parent)
    {
        setMinimumHeight(118);
        setAccessibleName(QStringLiteral("Numeric field history chart"));
    }

    void setData(const HistoryPage &page, const QString &fieldId)
    {
        m_values.clear();
        setVisible(!fieldId.isEmpty());
        if (fieldId.isEmpty()) return;
        for (auto event = page.events.crbegin(); event != page.events.crend(); ++event) {
            for (const HistoryChange &change : event->changes) {
                if (change.fieldId == fieldId && change.after.isDouble()) m_values.append(change.after.toDouble());
            }
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(QStringLiteral("#fffaf0")));
        painter.setPen(QColor(QStringLiteral("#30233d")));
        if (m_values.isEmpty()) {
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No numeric values on this page."));
            return;
        }
        const auto [lowIt, highIt] = std::minmax_element(m_values.cbegin(), m_values.cend());
        const double low = *lowIt;
        const double high = *highIt;
        const QRectF plot = QRectF(rect()).adjusted(12, 24, -12, -12);
        painter.drawText(QRectF(12, 2, width() - 24, 20), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Page range  %1 to %2").arg(QString::number(low), QString::number(high)));
        painter.drawRect(plot);
        QPolygonF points;
        const double scale = std::max(std::abs(low), std::abs(high));
        for (qsizetype i = 0; i < m_values.size(); ++i) {
            const double x = m_values.size() == 1 ? 0.5 : double(i) / double(m_values.size() - 1);
            const double y = low == high ? 0.5 : (m_values.at(i) / scale - low / scale) / (high / scale - low / scale);
            points.append(QPointF(plot.left() + plot.width() * x, plot.bottom() - plot.height() * y));
        }
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(QStringLiteral("#7657a7")), 2));
        painter.drawPolyline(points);
        painter.setBrush(painter.pen().color());
        for (const QPointF &point : points) painter.drawEllipse(point, 3, 3);
    }

private:
    QVector<double> m_values;
};

} // namespace

void showObjectHistory(QWidget *parent, const GameRepository &repository, const QString &objectId, const QString &name)
{
    QString error;
    const auto fields = repository.historyFields(objectId, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Could not load object history"), error);
        return;
    }
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("objectHistoryDialog"));
    dialog.setWindowTitle(QStringLiteral("Object History — %1").arg(name));
    dialog.resize(860, 620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *event = new QComboBox(&dialog);
    auto *field = new QComboBox(&dialog);
    auto *chartField = new QComboBox(&dialog);
    event->addItem(QStringLiteral("All events"));
    for (const auto &[key, label] : eventTypes) event->addItem(label, key);
    field->addItem(QStringLiteral("All fields"));
    chartField->addItem(QStringLiteral("Hide chart"));
    QMap<QString, int> nameCounts;
    for (const QString &label : fields) ++nameCounts[label];
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        const QString label = nameCounts.value(it.value()) == 1 ? it.value() : QStringLiteral("%1 (%2)").arg(it.value(), it.key());
        field->addItem(label, it.key());
        chartField->addItem(label, it.key());
    }
    auto *since = new QLineEdit(&dialog);
    auto *until = new QLineEdit(&dialog);
    since->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
    until->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
    form->addRow(QStringLiteral("Event"), event);
    form->addRow(QStringLiteral("Field"), field);
    form->addRow(QStringLiteral("From date (UTC)"), since);
    form->addRow(QStringLiteral("Through date (UTC)"), until);
    auto *apply = new QPushButton(QStringLiteral("Apply filters"), &dialog);
    form->addRow(apply);
    form->addRow(QStringLiteral("Chart field"), chartField);
    layout->addLayout(form);

    auto *table = new QTableWidget(0, 5, &dialog);
    table->setObjectName(QStringLiteral("historyTimeline"));
    table->setHorizontalHeaderLabels({QStringLiteral("Time (UTC)"), QStringLiteral("Event"),
        QStringLiteral("Field"), QStringLiteral("Before"), QStringLiteral("After")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setTextElideMode(Qt::ElideNone);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(table, 1);
    auto *chart = new HistoryChart(&dialog);
    layout->addWidget(chart);
    auto *footer = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *previous = footer->addButton(QStringLiteral("Previous"), QDialogButtonBox::ActionRole);
    auto *next = footer->addButton(QStringLiteral("Next"), QDialogButtonBox::ActionRole);
    auto *status = new QLabel(&dialog);
    status->setTextFormat(Qt::PlainText);
    auto *navigation = new QHBoxLayout;
    navigation->addWidget(status, 1);
    navigation->addWidget(footer);
    layout->addLayout(navigation);

    HistoryFilter activeFilter;
    HistoryPage currentPage;
    const auto updateChart = [&] { chart->setData(currentPage, chartField->currentData().toString()); };
    const auto loadPage = [&](const HistoryFilter &filter) {
        QString queryError;
        const HistoryPage page = repository.objectHistory(objectId, filter, &queryError);
        if (!queryError.isEmpty()) {
            status->setText(queryError);
            return;
        }
        activeFilter = filter;
        currentPage = page;
        table->setRowCount(0);
        const auto addRow = [&](const QStringList &cells) {
            const int row = table->rowCount();
            table->insertRow(row);
            for (int column = 0; column < cells.size(); ++column)
                table->setItem(row, column, new QTableWidgetItem(cells.at(column)));
        };
        for (const HistoryEvent &entry : page.events) {
            const auto type = std::find_if(eventTypes.cbegin(), eventTypes.cend(),
                                           [&](const auto &pair) { return pair.first == entry.type; });
            const QString label = type == eventTypes.cend() ? entry.type : type->second;
            if (entry.changes.isEmpty()) addRow({entry.timestamp, label, {}, {}, {}});
            for (const HistoryChange &change : entry.changes) {
                const QString fieldLabel = change.beforeFieldType.isEmpty() ? change.fieldName
                    : QStringLiteral("%1 (%2 → %3)").arg(change.fieldName, change.beforeFieldType, change.fieldType);
                addRow({entry.timestamp, label, fieldLabel, valueLabel(change.before), valueLabel(change.after)});
            }
        }
        table->resizeRowsToContents();
        table->scrollToTop();
        status->setText(page.events.isEmpty() ? QStringLiteral("No matching history events.")
                                             : QStringLiteral("Page %1 · %2 events").arg(filter.page + 1).arg(page.events.size()));
        previous->setEnabled(filter.page > 0);
        next->setEnabled(page.hasMore);
        updateChart();
    };
    previous->setEnabled(false);
    next->setEnabled(false);
    QObject::connect(footer, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(chartField, &QComboBox::currentIndexChanged, &dialog, updateChart);
    QObject::connect(previous, &QPushButton::clicked, &dialog, [&] {
        HistoryFilter filter = activeFilter;
        --filter.page;
        loadPage(filter);
    });
    QObject::connect(next, &QPushButton::clicked, &dialog, [&] {
        HistoryFilter filter = activeFilter;
        ++filter.page;
        loadPage(filter);
    });
    QObject::connect(apply, &QPushButton::clicked, &dialog, [&] {
        const QString start = since->text().trimmed();
        const QString end = until->text().trimmed();
        HistoryFilter filter{event->currentData().toString(), field->currentData().toString(),
                             QDate::fromString(start, Qt::ISODate), QDate::fromString(end, Qt::ISODate)};
        if ((!start.isEmpty() && !filter.since.isValid()) || (!end.isEmpty() && !filter.until.isValid()) ||
            (filter.since.isValid() && filter.until.isValid() && filter.since > filter.until)) {
            status->setText(QStringLiteral("Enter a valid date range in YYYY-MM-DD format."));
            return;
        }
        loadPage(filter);
    });
    updateChart();
    loadPage(activeFilter);
    dialog.exec();
}

} // namespace gmst
