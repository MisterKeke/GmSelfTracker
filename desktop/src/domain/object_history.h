#pragma once

#include <QDate>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QVector>

namespace gmst {

struct HistoryChange {
    QString fieldId;
    QString fieldName;
    QString fieldType;
    QString beforeFieldType;
    QJsonValue before = QJsonValue::Undefined;
    QJsonValue after = QJsonValue::Undefined;
};

struct HistoryEvent {
    QString id;
    QString type;
    QString timestamp;
    QVector<HistoryChange> changes;
};

struct HistoryFilter {
    QString eventType;
    QString fieldId;
    QDate since;
    QDate until;
    int page = 0;
    static constexpr int pageSize = 20;
};

struct HistoryPage {
    QVector<HistoryEvent> events;
    bool hasMore = false;
};

} // namespace gmst
