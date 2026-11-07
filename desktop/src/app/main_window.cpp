#include "app/main_window.h"

#include "app/theme.h"
#include "app/web_import_dialog.h"
#include "infrastructure/database/game_repository.h"
#include "domain/formula_evaluator.h"
#include "infrastructure/assets/image_decoder.h"
#include "presentation/widgets/object_history_dialog.h"
#include "presentation/widgets/sky_banner.h"

#include <QButtonGroup>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDoubleValidator>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QIcon>
#include <QListWidget>
#include <QMap>
#include <QMessageBox>
#include <QMenu>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QPalette>
#include <QProgressBar>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSet>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextEdit>
#include <QValidator>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>

namespace gmst {
namespace {

QFrame *panel(const QString &name, QWidget *parent = nullptr)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(name);
    return frame;
}

QLabel *textLabel(const QString &text, const QString &objectName = {}, QWidget *parent = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    if (!objectName.isEmpty()) label->setObjectName(objectName);
    return label;
}

QPushButton *button(const QString &text, const QString &objectName = {}, QWidget *parent = nullptr)
{
    auto *result = new QPushButton(text, parent);
    if (!objectName.isEmpty()) result->setObjectName(objectName);
    return result;
}

QVector<FieldDefinition> prioritizedCardFields(const ObjectType &type)
{
    QVector<FieldDefinition> fields;
    if (!type.primaryDisplayFieldId.isEmpty()) {
        const auto primary = std::find_if(type.fields.cbegin(), type.fields.cend(), [&](const FieldDefinition &field) {
            return field.id == type.primaryDisplayFieldId && !field.archived;
        });
        if (primary != type.fields.cend()) fields.append(*primary);
    }
    for (const FieldDefinition &field : type.fields)
        if (!field.archived && field.id != type.primaryDisplayFieldId) fields.append(field);
    return fields;
}

void addDialogButtons(QVBoxLayout *layout, QDialog *dialog, QPushButton **saveButton = nullptr)
{
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto *save = buttons->addButton(QStringLiteral("Save"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("goldButton"));
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    layout->addWidget(buttons);
    if (saveButton) *saveButton = save;
}

} // namespace

MainWindow::MainWindow(GameRepository &repository, QWidget *parent)
    : QMainWindow(parent), m_repository(repository)
{
    setWindowTitle(QStringLiteral("GmSelfTracker — Track · Plan · Explore · Collect"));
    setMinimumSize(1050, 680);
    resize(1600, 950);
    buildUi();
    refreshGames();
}

void MainWindow::buildUi()
{
    auto *frame = new QWidget(this);
    frame->setObjectName(QStringLiteral("appFrame"));
    auto *outer = new QVBoxLayout(frame);
    outer->setContentsMargins(8, 8, 8, 7);
    outer->setSpacing(7);

    auto *topbar = panel(QStringLiteral("topbar"), frame);
    topbar->setMinimumHeight(78);
    auto *topLayout = new QHBoxLayout(topbar);
    topLayout->setContentsMargins(9, 5, 8, 5);
    topLayout->setSpacing(10);

    auto *brand = new QWidget(topbar);
    brand->setMinimumWidth(250);
    brand->setMaximumWidth(385);
    auto *brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(0, 0, 0, 0);
    brandLayout->setSpacing(10);
    auto *brandMark = textLabel(QStringLiteral("🎮"));
    brandMark->setAlignment(Qt::AlignCenter);
    brandMark->setFixedSize(52, 48);
    brandMark->setStyleSheet(QStringLiteral("font-size: 31px; color: #c8aaff;"));
    auto *brandCopy = new QWidget(brand);
    auto *brandCopyLayout = new QVBoxLayout(brandCopy);
    brandCopyLayout->setContentsMargins(0, 0, 0, 0);
    brandCopyLayout->setSpacing(2);
    brandCopyLayout->addWidget(textLabel(QStringLiteral("GAME TRACKER ✦"), QStringLiteral("brandTitle")));
    brandCopyLayout->addWidget(textLabel(QStringLiteral("✦ TRACK · PLAN · EXPLORE · COLLECT ✦"), QStringLiteral("brandSubtitle")));
    brandLayout->addWidget(brandMark);
    brandLayout->addWidget(brandCopy, 1);
    topLayout->addWidget(brand);

    auto *topSky = new SkyBanner(topbar);
    topSky->setMinimumHeight(55);
    topLayout->addWidget(topSky, 1);

    auto *profile = new QWidget(topbar);
    auto *profileLayout = new QHBoxLayout(profile);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    profileLayout->setSpacing(7);
    auto *avatar = textLabel(QStringLiteral("🧙"));
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(40, 40);
    avatar->setStyleSheet(QStringLiteral("background:#ac755a;border:3px solid #f5d277; font-size:22px;"));
    auto *profileMeta = new QWidget(profile);
    auto *profileMetaLayout = new QVBoxLayout(profileMeta);
    profileMetaLayout->setContentsMargins(0, 0, 0, 0);
    profileMetaLayout->setSpacing(0);
    profileMetaLayout->addWidget(textLabel(m_repository.activeUsername(), QStringLiteral("activeAccountName")));
    profileMetaLayout->addWidget(textLabel(QStringLiteral("▣ My Profile"), QStringLiteral("mutedText")));
    auto *profileButton = button(QString(), QStringLiteral("iconButton"));
    profileButton->setObjectName(QStringLiteral("profileButton"));
    profileButton->setText(QStringLiteral("↪"));
    profileButton->setToolTip(QStringLiteral("Local profile"));
    auto *settingsButton = button(QStringLiteral("⚙"), QStringLiteral("iconButton"));
    settingsButton->setObjectName(QStringLiteral("settingsButton"));
    settingsButton->setToolTip(QStringLiteral("Settings"));
    profileLayout->addWidget(avatar);
    profileLayout->addWidget(profileMeta);
    profileLayout->addWidget(profileButton);
    profileLayout->addWidget(settingsButton);
    topLayout->addWidget(profile);
    connect(profileButton, &QPushButton::clicked, this, &MainWindow::openProfile);
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::openProfile);
    outer->addWidget(topbar);

    auto *layout = new QHBoxLayout();
    layout->setSpacing(7);
    layout->addWidget(makeSidebar());
    layout->addWidget(makeGameWorkspace(), 1);
    outer->addLayout(layout, 1);
    setCentralWidget(frame);

}

QWidget *MainWindow::makeSidebar()
{
    auto *side = panel(QStringLiteral("sidebar"));
    side->setFixedWidth(260);
    auto *layout = new QVBoxLayout(side);
    layout->setContentsMargins(10, 10, 10, 9);
    layout->setSpacing(9);

    auto *heading = new QWidget(side);
    heading->setObjectName(QStringLiteral("gameHeading"));
    heading->setStyleSheet(QStringLiteral("QWidget#gameHeading { background:#422f5e; border:2px solid #ac89cf; }"));
    auto *headingLayout = new QHBoxLayout(heading);
    headingLayout->setContentsMargins(9, 6, 9, 6);
    headingLayout->addWidget(textLabel(QStringLiteral("✦  MY GAMES"), QStringLiteral("pixelHeading")));
    m_gameCount = textLabel(QStringLiteral("0"), QStringLiteral("pixelHeading"));
    headingLayout->addWidget(m_gameCount, 0, Qt::AlignRight);
    layout->addWidget(heading);

    m_gameSearch = new QLineEdit(side);
    m_gameSearch->setPlaceholderText(QStringLiteral("Find a game…"));
    layout->addWidget(m_gameSearch);

    auto *listScroll = new QScrollArea(side);
    listScroll->setWidgetResizable(true);
    listScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QPalette listPalette = listScroll->viewport()->palette();
    listPalette.setColor(QPalette::Window, QColor(QStringLiteral("#2c2244")));
    listScroll->viewport()->setPalette(listPalette);
    listScroll->viewport()->setAutoFillBackground(true);
    m_gameList = new QWidget(listScroll);
    m_gameList->setAutoFillBackground(true);
    m_gameList->setPalette(listPalette);
    m_gameListLayout = new QVBoxLayout(m_gameList);
    m_gameListLayout->setContentsMargins(0, 0, 2, 0);
    m_gameListLayout->setSpacing(6);
    m_gameListLayout->addStretch(1);
    listScroll->setWidget(m_gameList);
    layout->addWidget(listScroll, 1);

    auto *addGame = button(QStringLiteral("＋  Add Game"), QStringLiteral("goldButton"), side);
    layout->addWidget(addGame);
    connect(addGame, &QPushButton::clicked, this, &MainWindow::openCreateGame);

    auto *templates = button(QStringLiteral("▣  Game Templates"), {}, side);
    layout->addWidget(templates);
    connect(templates, &QPushButton::clicked, this, [this] { selectPage(Page::Templates); });

    auto *spacer = new QWidget(side);
    spacer->setMinimumHeight(4);
    layout->addWidget(spacer, 1);

    auto *stats = panel(QStringLiteral(""), side);
    stats->setObjectName(QStringLiteral("quickStats"));
    stats->setStyleSheet(QStringLiteral("QFrame#quickStats { background:#261e39; border:2px solid #9e7eb8; }"));
    auto *statsLayout = new QVBoxLayout(stats);
    statsLayout->setContentsMargins(7, 6, 7, 6);
    statsLayout->setSpacing(3);
    statsLayout->addWidget(textLabel(QStringLiteral("▤  QUICK STATS"), QStringLiteral("pixelHeading")));
    auto addStat = [stats, statsLayout](const QString &name, QLabel **value) {
        auto *row = new QWidget(stats);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(textLabel(QStringLiteral("✧  ") + name));
        *value = textLabel(QStringLiteral("0"));
        (*value)->setStyleSheet(QStringLiteral("color:#ffe5a6;font-weight:900;"));
        rowLayout->addWidget(*value, 0, Qt::AlignRight);
        statsLayout->addWidget(row);
    };
    addStat(QStringLiteral("Games"), &m_gameTotalStat);
    addStat(QStringLiteral("Objects"), &m_objectTotalStat);
    addStat(QStringLiteral("Sections"), &m_sectionTotalStat);
    addStat(QStringLiteral("Goals done"), &m_goalDoneStat);
    layout->addWidget(stats);

    connect(m_gameSearch, &QLineEdit::textChanged, this, [this] { refreshGames(m_selectedGameId); });
    return side;
}

QWidget *MainWindow::makeGameWorkspace()
{
    auto *main = panel(QStringLiteral("gameArea"));
    auto *layout = new QVBoxLayout(main);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *hero = new SkyBanner(main);
    hero->setHeroMode(true);
    hero->setMinimumHeight(125);
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(22, 11, 12, 10);
    heroLayout->setSpacing(14);
    m_heroSymbol = textLabel(QStringLiteral("✧"));
    m_heroSymbol->setAlignment(Qt::AlignCenter);
    m_heroSymbol->setFixedSize(68, 68);
    m_heroSymbol->setStyleSheet(QStringLiteral("color:#fff1c7;background:#5b3f70;border:3px solid #ffdfa1;font-size:38px;"));
    auto *heroCopy = new QWidget(hero);
    auto *heroCopyLayout = new QVBoxLayout(heroCopy);
    heroCopyLayout->setContentsMargins(0, 0, 0, 0);
    heroCopyLayout->setSpacing(4);
    m_heroTitle = textLabel(QStringLiteral("GAME TRACKER"));
    m_heroTitle->setStyleSheet(QStringLiteral("color:#fff3cf;font:20px 'Courier New';font-weight:700;text-shadow:2px 2px #26304b;"));
    m_heroDescription = textLabel(QStringLiteral("Choose a game or create your first one."));
    heroCopyLayout->addWidget(m_heroTitle);
    heroCopyLayout->addWidget(m_heroDescription);
    heroLayout->addWidget(m_heroSymbol);
    heroLayout->addWidget(heroCopy, 1);
    m_editGameButton = button(QStringLiteral("✎  Edit"), QStringLiteral("softButton"), hero);
    heroLayout->addWidget(m_editGameButton, 0, Qt::AlignTop);
    connect(m_editGameButton, &QPushButton::clicked, this, &MainWindow::openEditGame);
    layout->addWidget(hero);

    auto *tabs = new QWidget(main);
    tabs->setObjectName(QStringLiteral("tabStrip"));
    tabs->setStyleSheet(QStringLiteral("QWidget#tabStrip { background:#392a54; border-bottom:3px solid #9673ae; }"));
    auto *tabsLayout = new QHBoxLayout(tabs);
    tabsLayout->setContentsMargins(8, 6, 8, 7);
    tabsLayout->setSpacing(5);
    m_tabGroup = new QButtonGroup(this);
    m_tabGroup->setExclusive(true);
    const QList<QPair<QString, Page>> entries = {
        {QStringLiteral("✦  Overview"), Page::Overview}, {QStringLiteral("◈  Objects"), Page::Objects},
        {QStringLiteral("◎  Goals"), Page::Goals}, {QStringLiteral("⚙  Types & Fields"), Page::Types},
        {QStringLiteral("▣  Templates"), Page::Templates}
    };
    int id = 0;
    for (const auto &entry : entries) {
        auto *tab = button(entry.first, QStringLiteral("tabButton"), tabs);
        tab->setCheckable(true);
        m_tabGroup->addButton(tab, id);
        tabsLayout->addWidget(tab);
        connect(tab, &QPushButton::clicked, this, [this, page = entry.second] { selectPage(page); });
        if (id == 0) tab->setChecked(true);
        ++id;
    }
    tabsLayout->addStretch(1);
    layout->addWidget(tabs);

    auto *body = new QWidget(main);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(8, 8, 8, 8);
    bodyLayout->setSpacing(8);

    auto *sectionPanel = panel(QStringLiteral(""), body);
    sectionPanel->setFixedWidth(210);
    sectionPanel->setObjectName(QStringLiteral("sectionPanel"));
    sectionPanel->setStyleSheet(QStringLiteral("QFrame#sectionPanel { background:#2c2244; border:0; border-right:2px solid #80659f; }"));
    auto *sectionLayout = new QVBoxLayout(sectionPanel);
    sectionLayout->setContentsMargins(3, 2, 4, 2);
    sectionLayout->setSpacing(8);
    auto *sectionHead = new QWidget(sectionPanel);
    auto *sectionHeadLayout = new QHBoxLayout(sectionHead);
    sectionHeadLayout->setContentsMargins(2, 1, 2, 1);
    sectionHeadLayout->addWidget(textLabel(QStringLiteral("▣  SECTIONS"), QStringLiteral("pixelHeading")));
    sectionHeadLayout->addStretch(1);
    auto *addSection = button(QStringLiteral("＋"), QStringLiteral("iconButton"), sectionHead);
    addSection->setFixedSize(32, 30);
    sectionHeadLayout->addWidget(addSection);
    sectionLayout->addWidget(sectionHead);
    auto *tip = textLabel(QStringLiteral("Choose a section before adding objects to place them here."), QStringLiteral("mutedText"));
    tip->setStyleSheet(QStringLiteral("font-size:11px;"));
    sectionLayout->addWidget(tip);
    m_sections = new QTreeWidget(sectionPanel);
    m_sections->setHeaderHidden(true);
    m_sections->setRootIsDecorated(false);
    m_sections->setIndentation(16);
    sectionLayout->addWidget(m_sections, 1);
    auto *rootDrop = textLabel(QStringLiteral("New objects use the selected section"));
    rootDrop->setAlignment(Qt::AlignCenter);
    rootDrop->setStyleSheet(QStringLiteral("border:2px dashed #9d7cb1;padding:8px;color:#e7d9f3;font-size:10px;"));
    sectionLayout->addWidget(rootDrop);
    connect(addSection, &QPushButton::clicked, this, &MainWindow::openCreateSection);
    connect(m_sections, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_selectedSectionId = item ? item->data(0, Qt::UserRole).toString() : QString();
        refreshObjectPage();
        selectPage(Page::Objects);
    });
    m_sections->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sections, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QTreeWidgetItem *item = m_sections->itemAt(position);
        if (!item || item->data(0, Qt::UserRole).toString().isEmpty()) return;
        const QString id = item->data(0, Qt::UserRole).toString();
        const QString name = item->text(0);
        QMenu menu(this);
        QAction *archive = menu.addAction(QStringLiteral("Archive section"));
        if (menu.exec(m_sections->viewport()->mapToGlobal(position)) != archive) return;
        if (QMessageBox::question(this, QStringLiteral("Archive section"),
            QStringLiteral("Archive %1? Its contents and placements will remain stored.").arg(name)) != QMessageBox::Yes) return;
        QString error;
        if (!m_repository.archiveSection(id, &error)) QMessageBox::warning(this, QStringLiteral("Could not archive section"), error);
        refreshGames(m_selectedGameId);
    });
    bodyLayout->addWidget(sectionPanel);

    m_pages = new QStackedWidget(body);
    m_pages->addWidget(makePage(QStringLiteral("YOUR GAME SPACE"),
        QStringLiteral("A quick view of recently edited objects and game statistics."),
        QStringLiteral("✦"), QStringLiteral("YOUR ADVENTURE STARTS HERE"),
        QStringLiteral("Create sections and add your first objects to begin building a game dashboard.")));
    m_pages->addWidget(makePage(QStringLiteral("OBJECT LIBRARY"),
        QStringLiteral("Browse every active object in this game."), QStringLiteral("◈"),
        QStringLiteral("NO OBJECTS YET"), QStringLiteral("Create an object type first, then add your first object.")));
    m_pages->addWidget(makePage(QStringLiteral("GOALS & DEVELOPMENT PLANS"),
        QStringLiteral("Manual milestones, checklists, numeric progress, and live field targets."), QStringLiteral("◎"),
        QStringLiteral("EVERY QUEST STARTS SOMEWHERE"), QStringLiteral("Create a manual goal, checklist, numeric target, or goal linked to a numeric object field.")));
    m_pages->addWidget(makePage(QStringLiteral("YOUR CUSTOM DATA TYPES"),
        QStringLiteral("Fields belong to a type and stay editable on each object."), QStringLiteral("⚙"),
        QStringLiteral("DEFINE YOUR COLLECTION"), QStringLiteral("Edit reusable object types and fields, preview formulas, and safely migrate saved values.")));
    m_pages->addWidget(makePage(QStringLiteral("GAME TEMPLATES"),
        QStringLiteral("Import and export game structures from reusable templates."), QStringLiteral("▣"),
        QStringLiteral("NO GAME TEMPLATES YET"), QStringLiteral("Template export, import, and instantiation have not been ported yet.")));
    auto *overviewHeading = m_pages->widget(0)->findChild<QFrame *>(QStringLiteral("pageHeading"));
    auto *overviewHeadingLayout = qobject_cast<QHBoxLayout *>(overviewHeading->layout());
    m_newObjectButton = button(QStringLiteral("＋  New Object"), QStringLiteral("goldButton"), overviewHeading);
    m_newObjectButton->setObjectName(QStringLiteral("newObjectButton"));
    m_newObjectButton->setEnabled(false);
    overviewHeadingLayout->addWidget(m_newObjectButton);
    connect(m_newObjectButton, &QPushButton::clicked, this, [this] { openObjectEditor(); });
    auto *objectHeading = m_pages->widget(1)->findChild<QFrame *>(QStringLiteral("pageHeading"));
    auto *objectHeadingLayout = qobject_cast<QHBoxLayout *>(objectHeading->layout());
    m_addSectionObjectButton = button(QStringLiteral("＋  New Object"), QStringLiteral("goldButton"), objectHeading);
    objectHeadingLayout->addWidget(m_addSectionObjectButton);
    connect(m_addSectionObjectButton, &QPushButton::clicked, this, [this] { openObjectEditor(); });
    auto *goalsHeading = m_pages->widget(2)->findChild<QFrame *>(QStringLiteral("pageHeading"));
    auto *goalsHeadingLayout = qobject_cast<QHBoxLayout *>(goalsHeading->layout());
    m_newGoalButton = button(QStringLiteral("＋  Add Goal"), QStringLiteral("goldButton"), goalsHeading);
    m_newGoalButton->setObjectName(QStringLiteral("newGoalButton"));
    m_newGoalButton->setEnabled(false);
    goalsHeadingLayout->addWidget(m_newGoalButton);
    connect(m_newGoalButton, &QPushButton::clicked, this, [this] { openGoalEditor(); });
    m_goalFilters = new QWidget(m_pages->widget(2));
    auto *goalFilterLayout = new QHBoxLayout(m_goalFilters);
    goalFilterLayout->setContentsMargins(0, 0, 0, 0);
    m_goalSearch = new QLineEdit(m_goalFilters);
    m_goalSearch->setObjectName(QStringLiteral("goalSearch"));
    m_goalSearch->setPlaceholderText(QStringLiteral("Search goals…"));
    m_goalSearch->setAccessibleName(QStringLiteral("Search goals"));
    m_goalSearch->setMaxLength(2000);
    goalFilterLayout->addWidget(m_goalSearch, 1);
    m_goalStatus = new QComboBox(m_goalFilters);
    m_goalStatus->setObjectName(QStringLiteral("goalStatusFilter"));
    m_goalStatus->setAccessibleName(QStringLiteral("Filter by status"));
    m_goalStatus->addItems({QStringLiteral("All statuses"), QStringLiteral("Active"), QStringLiteral("Completed")});
    goalFilterLayout->addWidget(m_goalStatus);
    m_goalType = new QComboBox(m_goalFilters);
    m_goalType->setAccessibleName(QStringLiteral("Filter by type"));
    m_goalType->addItem(QStringLiteral("All types"));
    m_goalType->addItem(QStringLiteral("Manual"), QStringLiteral("manual"));
    m_goalType->addItem(QStringLiteral("Checklist"), QStringLiteral("checklist"));
    m_goalType->addItem(QStringLiteral("Numeric"), QStringLiteral("numeric"));
    m_goalType->addItem(QStringLiteral("Field target"), QStringLiteral("field_target"));
    goalFilterLayout->addWidget(m_goalType);
    m_goalSort = new QComboBox(m_goalFilters);
    m_goalSort->setAccessibleName(QStringLiteral("Sort goals"));
    m_goalSort->addItems({QStringLiteral("Default order"), QStringLiteral("Title (A–Z)"), QStringLiteral("Newest created"),
                         QStringLiteral("Recently updated"), QStringLiteral("Highest progress"), QStringLiteral("Completed first")});
    goalFilterLayout->addWidget(m_goalSort);
    m_pageLayouts.at(2)->insertWidget(1, m_goalFilters);
    const auto changeGoalFilter = [this] {
        m_goalPage = 0;
        refreshGoalPage();
    };
    connect(m_goalSearch, &QLineEdit::textChanged, this, changeGoalFilter);
    for (auto *choice : {m_goalStatus, m_goalType, m_goalSort})
        connect(choice, &QComboBox::currentIndexChanged, this, changeGoalFilter);
    auto *typesHeading = m_pages->widget(3)->findChild<QFrame *>(QStringLiteral("pageHeading"));
    auto *typesHeadingLayout = qobject_cast<QHBoxLayout *>(typesHeading->layout());
    m_newTypeButton = button(QStringLiteral("＋  New Type"), QStringLiteral("goldButton"), typesHeading);
    m_newTypeButton->setObjectName(QStringLiteral("newTypeButton"));
    m_newTypeButton->setEnabled(false);
    typesHeadingLayout->addWidget(m_newTypeButton);
    connect(m_newTypeButton, &QPushButton::clicked, this, &MainWindow::openCreateType);
    bodyLayout->addWidget(m_pages, 1);
    layout->addWidget(body, 1);
    return main;
}

QWidget *MainWindow::makePage(const QString &title, const QString &description, const QString &icon,
                              const QString &emptyTitle, const QString &emptyDescription)
{
    auto *page = new QWidget();
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(9);

    auto *heading = panel(QStringLiteral(""), page);
    heading->setObjectName(QStringLiteral("pageHeading"));
    heading->setStyleSheet(QStringLiteral("QFrame#pageHeading { background:#463363; border:2px solid #b08bc9; }"));
    auto *headingLayout = new QHBoxLayout(heading);
    headingLayout->setContentsMargins(10, 7, 10, 7);
    headingLayout->setSpacing(12);
    auto *copy = new QWidget(heading);
    auto *copyLayout = new QVBoxLayout(copy);
    copyLayout->setContentsMargins(0, 0, 0, 0);
    copyLayout->setSpacing(1);
    copyLayout->addWidget(textLabel(icon + QStringLiteral("  ") + title, QStringLiteral("pixelHeading")));
    copyLayout->addWidget(textLabel(description, QStringLiteral("mutedText")));
    copyLayout->addStretch(1);
    headingLayout->addWidget(copy, 1);
    outer->addWidget(heading);

    auto *empty = panel(QStringLiteral(""), page);
    empty->setMinimumHeight(205);
    empty->setObjectName(QStringLiteral("emptyState"));
    empty->setStyleSheet(QStringLiteral("QFrame#emptyState { color:#30233d; background:#fff2d6; border:3px dashed #b18c68; }"));
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setContentsMargins(20, 18, 20, 18);
    emptyLayout->setSpacing(8);
    emptyLayout->addStretch(1);
    auto *largeIcon = textLabel(icon, QStringLiteral("inkText"));
    largeIcon->setAlignment(Qt::AlignCenter);
    largeIcon->setStyleSheet(QStringLiteral("font-size:38px;"));
    emptyLayout->addWidget(largeIcon);
    auto *emptyTitleLabel = textLabel(emptyTitle, QStringLiteral("inkText"));
    emptyTitleLabel->setObjectName(QStringLiteral("emptyTitleLabel"));
    emptyTitleLabel->setAlignment(Qt::AlignCenter);
    emptyTitleLabel->setStyleSheet(QStringLiteral("font:10px 'Courier New';font-weight:700;color:#563d66;"));
    emptyLayout->addWidget(emptyTitleLabel);
    auto *emptyText = textLabel(emptyDescription, QStringLiteral("inkText"));
    emptyText->setObjectName(QStringLiteral("emptyDescriptionLabel"));
    emptyText->setAlignment(Qt::AlignCenter);
    emptyText->setMaximumWidth(620);
    emptyText->setStyleSheet(QStringLiteral("color:#705b70;"));
    emptyLayout->addWidget(emptyText, 0, Qt::AlignHCenter);
    emptyLayout->addStretch(1);
    outer->addWidget(empty);
    outer->addStretch();
    m_pageLayouts.append(outer);
    m_emptyStates.append(empty);
    m_featureViews.append(nullptr);
    return page;
}

void MainWindow::refreshGames(const QString &preferredId)
{
    QString error;
    m_games = m_repository.games(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Could not load games"), error);
        return;
    }
    while (QLayoutItem *item = m_gameListLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    const QString filter = m_gameSearch->text().trimmed();
    int visibleCount = 0;
    for (const Game &game : std::as_const(m_games)) {
        if (!filter.isEmpty() && !game.name.contains(filter, Qt::CaseInsensitive)) continue;
        auto *gameButton = button(QStringLiteral("%1   %2").arg(game.icon, game.name), QStringLiteral("gameButton"), m_gameList);
        gameButton->setCheckable(true);
        gameButton->setChecked(game.id == m_selectedGameId);
        gameButton->setProperty("gameId", game.id);
        gameButton->setToolTip(game.description);
        m_gameListLayout->addWidget(gameButton);
        connect(gameButton, &QPushButton::clicked, this, [this, id = game.id] { selectGame(id); });
        ++visibleCount;
    }
    if (visibleCount == 0) {
        auto *empty = textLabel(m_games.isEmpty() ? QStringLiteral("No games yet.") : QStringLiteral("No games match your search."), QStringLiteral("mutedText"));
        empty->setStyleSheet(QStringLiteral("font-size:12px;padding:6px;"));
        m_gameListLayout->addWidget(empty);
    }
    m_gameListLayout->addStretch(1);
    m_gameCount->setText(QString::number(m_games.size()));
    m_gameTotalStat->setText(QString::number(m_games.size()));
    int objectTotal = 0;
    int sectionTotal = 0;
    int goalsTotal = 0;
    int goalsDone = 0;
    for (const Game &game : std::as_const(m_games)) {
        objectTotal += game.objectCount;
        sectionTotal += game.sectionCount;
        goalsTotal += game.goalCount;
        goalsDone += game.completedGoals;
    }
    m_objectTotalStat->setText(QString::number(objectTotal));
    m_sectionTotalStat->setText(QString::number(sectionTotal));
    m_goalDoneStat->setText(QStringLiteral("%1 / %2").arg(goalsDone).arg(goalsTotal));

    QString target = preferredId;
    if (!target.isEmpty() && std::none_of(m_games.cbegin(), m_games.cend(), [&](const Game &g) { return g.id == target; })) target.clear();
    if (target.isEmpty() && !m_games.isEmpty()) target = m_games.constFirst().id;
    selectGame(target);
}

void MainWindow::refreshSections()
{
    const QSignalBlocker blocker(m_sections);
    m_sections->clear();
    m_gameSections.clear();
    if (m_selectedGameId.isEmpty()) return;
    QString error;
    m_gameSections = m_repository.sections(m_selectedGameId, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Could not load sections"), error);
        return;
    }
    QMap<QString, QTreeWidgetItem *> itemById;
    QMap<QString, QString> parents;
    auto *allObjects = new QTreeWidgetItem(m_sections, {QStringLiteral("All objects")});
    for (const Section &section : m_gameSections) {
        auto *item = new QTreeWidgetItem();
        item->setText(0, QStringLiteral("%1   %2").arg(section.icon, section.name));
        item->setData(0, Qt::UserRole, section.id);
        item->setToolTip(0, section.name);
        itemById.insert(section.id, item);
        parents.insert(section.id, section.parentId);
    }
    for (const Section &section : m_gameSections) {
        QSet<QString> ancestors;
        QString parentId = section.parentId;
        while (itemById.contains(parentId) && !ancestors.contains(parentId)) {
            ancestors.insert(parentId);
            parentId = parents.value(parentId);
        }
        auto *item = itemById.value(section.id);
        auto *parent = itemById.value(section.parentId);
        if (parent && !ancestors.contains(parentId)) parent->addChild(item);
        else m_sections->addTopLevelItem(item);
    }
    if (!itemById.contains(m_selectedSectionId)) m_selectedSectionId.clear();
    m_sections->setCurrentItem(itemById.value(m_selectedSectionId, allObjects));
    m_sections->expandAll();
}

void MainWindow::refreshFeaturePages()
{
    QString error;
    if (m_selectedGameId.isEmpty()) {
        m_types.clear();
        m_objects.clear();
        m_goals.clear();
    } else {
        m_types = m_repository.objectTypes(m_selectedGameId, &error);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Could not load object types"), error);
            return;
        }
        m_objects = m_repository.objects(m_selectedGameId, &error);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Could not load objects"), error);
            return;
        }
        m_goals = m_repository.goals(m_selectedGameId, &error);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Could not load goals"), error);
            return;
        }
    }
    m_newObjectButton->setEnabled(!m_selectedGameId.isEmpty());
    m_addSectionObjectButton->setEnabled(!m_selectedGameId.isEmpty());
    m_newGoalButton->setEnabled(!m_selectedGameId.isEmpty());
    m_newTypeButton->setEnabled(!m_selectedGameId.isEmpty());

    auto setEmpty = [this](int page, const QString &title, const QString &description) {
        if (page < 0 || page >= m_emptyStates.size()) return;
        if (auto *label = m_emptyStates.at(page)->findChild<QLabel *>(QStringLiteral("emptyTitleLabel"))) label->setText(title);
        if (auto *label = m_emptyStates.at(page)->findChild<QLabel *>(QStringLiteral("emptyDescriptionLabel"))) label->setText(description);
    };

    setEmpty(0, QStringLiteral("YOUR ADVENTURE STARTS HERE"), m_selectedGameId.isEmpty()
        ? QStringLiteral("Create a game to start building your collection.")
        : m_objects.isEmpty() ? QStringLiteral("Create an object type, then add objects to build your game space.")
                              : QStringLiteral("Your game overview is ready."));
    setEmpty(3, QStringLiteral("DEFINE YOUR COLLECTION"), m_selectedGameId.isEmpty()
        ? QStringLiteral("Select a game before creating object types.")
        : QStringLiteral("Create a custom type, then add fields for the values you want to track."));
    for (const int pageIndex : {0, 3}) {
        if (m_featureViews.at(pageIndex)) {
            m_pageLayouts.at(pageIndex)->removeWidget(m_featureViews.at(pageIndex));
            delete m_featureViews.at(pageIndex);
            m_featureViews[pageIndex] = nullptr;
        }
    }

    if (!m_objects.isEmpty()) {
        auto *scroll = new QScrollArea(m_pages->widget(0));
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(QStringLiteral("QScrollArea { border:0;background:transparent; }"));
        auto *content = new QWidget(scroll);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(1, 1, 8, 8);
        contentLayout->setSpacing(9);

        auto *recent = panel(QStringLiteral("overviewRecent"), content);
        recent->setStyleSheet(QStringLiteral("QFrame#overviewRecent { background:#fff2d6;border:3px solid #c5a76f; }"));
        auto *recentLayout = new QVBoxLayout(recent);
        recentLayout->setContentsMargins(10, 8, 10, 8);
        recentLayout->setSpacing(6);
        auto *recentTitle = textLabel(QStringLiteral("Recently edited"));
        recentTitle->setStyleSheet(QStringLiteral("color:#372746;font-size:16px;font-weight:900;border-bottom:1px dotted #c5a76f;padding-bottom:5px;"));
        recentLayout->addWidget(recentTitle);
        auto *cards = new QGridLayout();
        cards->setContentsMargins(0, 0, 0, 0);
        cards->setSpacing(8);
        QMap<QString, QString> names;
        for (const ObjectRecord &item : std::as_const(m_objects)) names.insert(item.id, item.name);
        const int visibleObjects = std::min(4, static_cast<int>(m_objects.size()));
        for (int index = 0; index < visibleObjects; ++index) {
            const ObjectRecord &object = m_objects.at(index);
            const auto typeIt = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &value) { return value.id == object.typeId; });
            auto *card = panel(QStringLiteral("overviewObjectCard"), recent);
            card->setStyleSheet(QStringLiteral("QFrame#overviewObjectCard { background:#fff2d6;border:2px solid #c6a36c; }"));
            auto *cardLayout = new QHBoxLayout(card);
            cardLayout->setContentsMargins(6, 6, 6, 6);
            cardLayout->setSpacing(8);
            auto *art = panel(QStringLiteral("overviewObjectArt"), card);
            art->setFixedSize(82, 116);
            art->setStyleSheet(QStringLiteral("QFrame#overviewObjectArt { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #b68ee0,stop:.55 #674688,stop:1 #392c57);border:3px solid #77568b; }"));
            auto *artLayout = new QVBoxLayout(art);
            auto *mark = textLabel(object.typeName.left(1).toUpper());
            mark->setAlignment(Qt::AlignCenter);
            mark->setStyleSheet(QStringLiteral("color:#ffe9b1;font-size:38px;font-weight:700;"));
            QString artReference = object.image;
            if (artReference.isEmpty() && typeIt != m_types.cend()) artReference = typeIt->image;
            if (!artReference.isEmpty()) {
                const QImage artwork = m_repository.loadAssetImage(artReference);
                if (!artwork.isNull()) {
                    mark->setText({});
                    mark->setPixmap(QPixmap::fromImage(artwork).scaled(72, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                }
            }
            artLayout->addWidget(mark);
            cardLayout->addWidget(art);

            auto *details = new QWidget(card);
            auto *detailsLayout = new QVBoxLayout(details);
            detailsLayout->setContentsMargins(0, 0, 0, 0);
            detailsLayout->setSpacing(3);
            auto *objectName = textLabel(object.name);
            objectName->setStyleSheet(QStringLiteral("color:#372746;font-size:14px;font-weight:900;"));
            detailsLayout->addWidget(objectName);
            auto *type = textLabel(object.typeName);
            type->setStyleSheet(QStringLiteral("color:#735d75;font-size:10px;"));
            detailsLayout->addWidget(type);
            int shownFields = 0;
            if (typeIt != m_types.cend()) {
                for (const FieldDefinition &field : prioritizedCardFields(*typeIt)) {
                    if (shownFields >= 4) continue;
                    QString valueText;
                    if (field.fieldType == QStringLiteral("reference") || field.fieldType == QStringLiteral("references")) {
                        QStringList labels;
                        for (const QString &id : object.references.value(field.id)) labels.append(names.value(id, QStringLiteral("Archived object")));
                        valueText = labels.join(QStringLiteral(", "));
                    } else if (field.fieldType == QStringLiteral("formula") && object.formulaErrors.contains(field.id)) {
                        valueText = QStringLiteral("⚠ %1").arg(object.formulaErrors.value(field.id));
                    } else {
                        const QJsonValue value = object.values.value(field.id);
                        if (field.fieldType == QStringLiteral("progress") && value.isObject()) {
                            valueText = QStringLiteral("%1 / %2").arg(value.toObject().value(QStringLiteral("current")).toVariant().toString(),
                                value.toObject().value(QStringLiteral("max")).toVariant().toString());
                        } else if (value.isBool()) valueText = value.toBool() ? QStringLiteral("Yes") : QStringLiteral("No");
                        else if (!value.isNull() && !value.isUndefined()) valueText = value.toVariant().toString();
                    }
                    if (valueText.isEmpty()) continue;
                    auto *row = new QWidget(details);
                    auto *rowLayout = new QHBoxLayout(row);
                    rowLayout->setContentsMargins(0, 0, 0, 0);
                    auto *fieldName = textLabel(QStringLiteral("✦  %1").arg(field.name));
                    fieldName->setStyleSheet(QStringLiteral("color:#675370;font-size:9px;"));
                    auto *fieldValue = textLabel(valueText);
                    fieldValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    fieldValue->setStyleSheet(QStringLiteral("color:#282036;font-size:9px;font-weight:800;"));
                    rowLayout->addWidget(fieldName, 1);
                    rowLayout->addWidget(fieldValue, 1);
                    detailsLayout->addWidget(row);
                    ++shownFields;
                }
            }
            auto *actions = new QHBoxLayout();
            auto *edit = button(QStringLiteral("✎ Edit"), QStringLiteral("softButton"), details);
            auto *archive = button(QStringLiteral("Archive"), QStringLiteral("softButton"), details);
            actions->addWidget(edit);
            actions->addWidget(archive);
            auto *history = button(QStringLiteral("History"), QStringLiteral("objectHistoryButton"), details);
            actions->addWidget(history);
            connect(history, &QPushButton::clicked, this, [this, id = object.id, name = object.name] {
                showObjectHistory(this, m_repository, id, name);
            });
            actions->addStretch(1);
            detailsLayout->addLayout(actions);
            connect(edit, &QPushButton::clicked, this, [this, id = object.id] { openObjectEditor(id); });
            connect(archive, &QPushButton::clicked, this, [this, id = object.id, name = object.name] {
                if (QMessageBox::question(this, QStringLiteral("Archive object"), QStringLiteral("Archive %1?").arg(name)) != QMessageBox::Yes) return;
                QString archiveError;
                if (!m_repository.archiveObject(id, &archiveError)) QMessageBox::warning(this, QStringLiteral("Could not archive object"), archiveError);
                else refreshGames(m_selectedGameId);
            });
            cardLayout->addWidget(details, 1);
            cards->addWidget(card, index / 2, index % 2);
        }
        recentLayout->addLayout(cards);

        auto *stats = panel(QStringLiteral("overviewStats"), content);
        stats->setStyleSheet(QStringLiteral("QFrame#overviewStats { background:#fff2d6;border:3px solid #c5a76f; }"));
        auto *statsLayout = new QVBoxLayout(stats);
        statsLayout->setContentsMargins(10, 8, 10, 8);
        auto *statsTitle = textLabel(QStringLiteral("Game statistics"));
        statsTitle->setStyleSheet(QStringLiteral("color:#372746;font-size:15px;font-weight:900;border-bottom:1px dotted #c5a76f;padding-bottom:5px;"));
        statsLayout->addWidget(statsTitle);
        auto *statGrid = new QGridLayout();
        statGrid->setContentsMargins(0, 0, 0, 0);
        statGrid->setSpacing(6);
        const Game game = currentGame();
        const QVector<QPair<QString, QString>> values = {
            {QStringLiteral("Objects"), QString::number(game.objectCount)},
            {QStringLiteral("Sections"), QString::number(game.sectionCount)},
            {QStringLiteral("Goals"), QString::number(game.goalCount)},
            {QStringLiteral("Goals complete"), QStringLiteral("%1 / %2").arg(game.completedGoals).arg(game.goalCount)}
        };
        for (int index = 0; index < values.size(); ++index) {
            auto *tile = panel(QStringLiteral("overviewStatTile"), stats);
            tile->setStyleSheet(QStringLiteral("QFrame#overviewStatTile { background:#f4ddb0;border:2px solid #c5a76f; }"));
            auto *tileLayout = new QVBoxLayout(tile);
            tileLayout->setContentsMargins(7, 5, 7, 5);
            auto *label = textLabel(values.at(index).first);
            label->setStyleSheet(QStringLiteral("color:#735d75;font-size:10px;"));
            tileLayout->addWidget(label);
            auto *amount = textLabel(values.at(index).second);
            amount->setStyleSheet(QStringLiteral("color:#30233d;font-size:17px;font-weight:900;"));
            tileLayout->addWidget(amount);
            statGrid->addWidget(tile, 0, index);
        }
        statsLayout->addLayout(statGrid);
        contentLayout->addWidget(recent);
        contentLayout->addWidget(stats);
        contentLayout->addStretch(1);
        scroll->setWidget(content);
        m_pageLayouts.at(0)->insertWidget(1, scroll, 1);
        m_emptyStates.at(0)->hide();
        m_featureViews[0] = scroll;
    } else {
        m_emptyStates.at(0)->show();
    }

    refreshObjectPage();

    const QVector<ObjectType> managedTypes = m_repository.objectTypes(m_selectedGameId, nullptr, true);
    if (!managedTypes.isEmpty()) {
        auto *scroll = new QScrollArea(m_pages->widget(3));
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(QStringLiteral("QScrollArea { border:0;background:transparent; }"));
        auto *content = new QWidget(scroll);
        auto *grid = new QGridLayout(content);
        grid->setContentsMargins(1, 1, 8, 8);
        grid->setSpacing(10);
        int index = 0;
        for (const ObjectType &type : std::as_const(managedTypes)) {
            auto *card = panel(QStringLiteral("typeCard"), content);
            card->setStyleSheet(QStringLiteral("QFrame#typeCard { background:#fff2d6;border:3px solid #c5a76f; }"));
            auto *cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(10, 9, 10, 9);
            cardLayout->setSpacing(5);
            auto *title = textLabel(QStringLiteral("%1  %2%3").arg(type.icon, type.name,
                type.archived ? QStringLiteral("  ·  ARCHIVED") : QString()));
            title->setStyleSheet(QStringLiteral("color:#372746;font-size:15px;font-weight:900;"));
            cardLayout->addWidget(title);
            if (!type.description.isEmpty()) {
                auto *description = textLabel(type.description);
                description->setStyleSheet(QStringLiteral("color:#705b70;font-size:11px;"));
                cardLayout->addWidget(description);
            }
            if (!type.archived) {
                auto *primaryRow = new QWidget(card);
                auto *primaryLayout = new QHBoxLayout(primaryRow);
                primaryLayout->setContentsMargins(0, 2, 0, 2);
                primaryLayout->addWidget(textLabel(QStringLiteral("Primary display")));
                auto *primaryChoice = new QComboBox(primaryRow);
                primaryChoice->setObjectName(QStringLiteral("primaryDisplayFieldChoice"));
                primaryChoice->addItem(QStringLiteral("None"), QString());
                for (const FieldDefinition &field : type.fields)
                    if (!field.archived) primaryChoice->addItem(field.icon + QStringLiteral("  ") + field.name, field.id);
                const int primaryIndex = primaryChoice->findData(type.primaryDisplayFieldId);
                primaryChoice->setCurrentIndex(std::max(0, primaryIndex));
                primaryLayout->addWidget(primaryChoice, 1);
                auto *savePrimary = button(QStringLiteral("Save"), QStringLiteral("softButton"), primaryRow);
                primaryLayout->addWidget(savePrimary);
                connect(savePrimary, &QPushButton::clicked, this, [this, id = type.id, primaryChoice] {
                    QString error;
                    if (!m_repository.setPrimaryDisplayField(id, primaryChoice->currentData().toString(), &error))
                        QMessageBox::warning(this, QStringLiteral("Could not update primary field"), error);
                    else refreshGames(m_selectedGameId);
                });
                cardLayout->addWidget(primaryRow);
            }
            for (const FieldDefinition &field : type.fields) {
                auto *fieldRow = new QWidget(card);
                fieldRow->setObjectName(QStringLiteral("typeFieldRow"));
                auto *rowLayout = new QHBoxLayout(fieldRow);
                rowLayout->setContentsMargins(0, 2, 0, 2);
                auto *fieldTitle = textLabel(QStringLiteral("%1  %2%3").arg(field.icon, field.name,
                    field.archived ? QStringLiteral("  ·  ARCHIVED") : QString()));
                fieldTitle->setObjectName(QStringLiteral("typeFieldRowTitle"));
                fieldTitle->setProperty("fieldId", field.id);
                rowLayout->addWidget(fieldTitle, 1);
                auto *kind = textLabel(field.fieldType.toUpper());
                kind->setStyleSheet(QStringLiteral("color:#735d75;font-size:9px;"));
                rowLayout->addWidget(kind);
                auto *editField = button(QStringLiteral("Edit"), QStringLiteral("softButton"), fieldRow);
                editField->setObjectName(QStringLiteral("editFieldButton"));
                editField->setEnabled(!field.archived && !type.archived);
                rowLayout->addWidget(editField);
                connect(editField, &QPushButton::clicked, this, [this, typeId = type.id, fieldId = field.id] {
                    openEditField(typeId, fieldId);
                });
                auto *migrateField = button(QStringLiteral("Change Type…"), QStringLiteral("softButton"), fieldRow);
                migrateField->setObjectName(QStringLiteral("migrateFieldTypeButton"));
                migrateField->setEnabled(!field.archived && !type.archived);
                rowLayout->addWidget(migrateField);
                connect(migrateField, &QPushButton::clicked, this, [this, typeId = type.id, fieldId = field.id] {
                    openFieldTypeMigration(typeId, fieldId);
                });
                auto *archiveField = button(field.archived ? QStringLiteral("Restore") : QStringLiteral("Archive"),
                    QStringLiteral("softButton"), fieldRow);
                archiveField->setEnabled(!type.archived);
                rowLayout->addWidget(archiveField);
                connect(archiveField, &QPushButton::clicked, this, [this, field] {
                    const bool archive = !field.archived;
                    if (archive && QMessageBox::question(this, QStringLiteral("Archive field"),
                        QStringLiteral("Archive %1? Its saved values will be preserved. Any active dependency must be updated first.").arg(field.name)) != QMessageBox::Yes) return;
                    QString error;
                    if (!m_repository.setFieldArchived(field.id, archive, &error))
                        QMessageBox::warning(this, archive ? QStringLiteral("Could not archive field") : QStringLiteral("Could not restore field"), error);
                    else refreshGames(m_selectedGameId);
                });
                cardLayout->addWidget(fieldRow);
            }
            auto *actions = new QHBoxLayout();
            if (!type.archived) {
                auto *editType = button(QStringLiteral("Edit Type"), QStringLiteral("softButton"), card);
                actions->addWidget(editType);
                connect(editType, &QPushButton::clicked, this, [this, id = type.id] { openEditType(id); });
                auto *archiveType = button(QStringLiteral("Archive Type"), QStringLiteral("softButton"), card);
                actions->addWidget(archiveType);
                connect(archiveType, &QPushButton::clicked, this, [this, type] {
                    if (QMessageBox::question(this, QStringLiteral("Archive object type"),
                        QStringLiteral("Archive %1? Its objects, values, and relationships will be kept.").arg(type.name)) != QMessageBox::Yes) return;
                    QString error;
                    if (!m_repository.setObjectTypeArchived(type.id, true, &error))
                        QMessageBox::warning(this, QStringLiteral("Could not archive type"), error);
                    else refreshGames(m_selectedGameId);
                });
                auto *addField = button(QStringLiteral("＋ Add Field"), QStringLiteral("softButton"), card);
                actions->addWidget(addField);
                connect(addField, &QPushButton::clicked, this, [this, id = type.id] { openCreateField(id); });
            } else {
                auto *restoreType = button(QStringLiteral("Restore Type"), QStringLiteral("softButton"), card);
                actions->addWidget(restoreType);
                connect(restoreType, &QPushButton::clicked, this, [this, type] {
                    QString error;
                    if (!m_repository.setObjectTypeArchived(type.id, false, &error))
                        QMessageBox::warning(this, QStringLiteral("Could not restore type"), error);
                    else refreshGames(m_selectedGameId);
                });
            }
            actions->addStretch(1);
            cardLayout->addLayout(actions);
            grid->addWidget(card, index / 2, index % 2);
            ++index;
        }
        grid->setRowStretch((index + 1) / 2, 1);
        scroll->setWidget(content);
        m_pageLayouts.at(3)->insertWidget(1, scroll, 1);
        m_emptyStates.at(3)->hide();
        m_featureViews[3] = scroll;
    } else {
        m_emptyStates.at(3)->show();
    }

    refreshGoalPage();
}

void MainWindow::refreshGoalPage()
{
    if (m_featureViews.at(2)) {
        delete m_featureViews.at(2);
        m_featureViews[2] = nullptr;
    }
    m_goalFilters->setVisible(!m_goals.isEmpty());
    const QString search = m_goalSearch->text().trimmed();
    const QString type = m_goalType->currentData().toString();
    const int status = m_goalStatus->currentIndex();
    QVector<const Goal *> matches;
    for (const Goal &goal : std::as_const(m_goals)) {
        if (!search.isEmpty() && !(goal.title + QLatin1Char(' ') + goal.description).contains(search, Qt::CaseInsensitive))
            continue;
        if (!type.isEmpty() && goal.goalType != type) continue;
        if ((status == 1 && goal.completed) || (status == 2 && !goal.completed)) continue;
        matches.append(&goal);
    }
    const int sort = m_goalSort->currentIndex();
    if (sort > 0) {
        std::stable_sort(matches.begin(), matches.end(), [sort](const Goal *left, const Goal *right) {
            switch (sort) {
            case 1: return QString::localeAwareCompare(left->title.toCaseFolded(), right->title.toCaseFolded()) < 0;
            case 2: return left->createdAt > right->createdAt;
            case 3: return left->updatedAt > right->updatedAt;
            case 4: return left->progress > right->progress;
            case 5: return left->completed > right->completed;
            default: return false;
            }
        });
    }
    constexpr int pageSize = 20;
    const int pageCount = std::max(1, (static_cast<int>(matches.size()) + pageSize - 1) / pageSize);
    m_goalPage = std::clamp(m_goalPage, 0, pageCount - 1);
    if (!matches.isEmpty()) {
        auto *scroll = new QScrollArea(m_pages->widget(2));
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(QStringLiteral("QScrollArea { border:0;background:transparent; }"));
        auto *content = new QWidget(scroll);
        auto *list = new QVBoxLayout(content);
        content->setObjectName(QStringLiteral("goalList"));
        list->setContentsMargins(1, 1, 8, 8);
        list->setSpacing(8);
        const qsizetype end = std::min<qsizetype>((m_goalPage + 1) * pageSize, matches.size());
        for (qsizetype index = m_goalPage * pageSize; index < end; ++index) {
            const Goal &goal = *matches.at(index);
            auto *card = panel(QStringLiteral("goalCard"), content);
            card->setStyleSheet(QStringLiteral("QFrame#goalCard { background:#fff2d6;border:3px solid #c5a76f; }"));
            auto *cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(11, 8, 11, 8);
            cardLayout->setSpacing(5);

            auto *top = new QWidget(card);
            auto *topLayout = new QHBoxLayout(top);
            topLayout->setContentsMargins(0, 0, 0, 0);
            auto *title = textLabel(QStringLiteral("◎  %1").arg(goal.title));
            title->setStyleSheet(QStringLiteral("color:#372746;font-size:15px;font-weight:900;"));
            topLayout->addWidget(title, 1);
            auto *state = textLabel(goal.completed ? QStringLiteral("✓ COMPLETE") :
                goal.ready ? QStringLiteral("IN PROGRESS") : QStringLiteral("LOCKED"));
            state->setStyleSheet(goal.completed
                ? QStringLiteral("color:#287748;font-size:10px;font-weight:900;")
                : goal.ready ? QStringLiteral("color:#735d75;font-size:10px;font-weight:900;")
                             : QStringLiteral("color:#9b4845;font-size:10px;font-weight:900;"));
            topLayout->addWidget(state);
            cardLayout->addWidget(top);

            if (!goal.description.isEmpty()) {
                auto *description = textLabel(goal.description);
                description->setStyleSheet(QStringLiteral("color:#705b70;font-size:11px;"));
                cardLayout->addWidget(description);
            }
            auto *progressRow = new QWidget(card);
            auto *progressLayout = new QHBoxLayout(progressRow);
            progressLayout->setContentsMargins(0, 0, 0, 0);
            auto *progress = new QProgressBar(progressRow);
            progress->setObjectName(QStringLiteral("goalProgressBar"));
            progress->setRange(0, 100);
            progress->setValue(qBound(0, qRound(goal.progress), 100));
            progress->setFormat(QStringLiteral("%p%"));
            progressLayout->addWidget(progress, 1);
            const QString kind = goal.goalType == QStringLiteral("field_target") ? QStringLiteral("Field target")
                : goal.goalType == QStringLiteral("numeric") ? QStringLiteral("Numeric")
                : goal.goalType == QStringLiteral("checklist") ? QStringLiteral("Checklist") : QStringLiteral("Manual");
            auto *kindLabel = textLabel(kind);
            kindLabel->setStyleSheet(QStringLiteral("color:#735d75;font-size:10px;"));
            progressLayout->addWidget(kindLabel);
            cardLayout->addWidget(progressRow);

            if (!goal.ready && !goal.blockedBy.isEmpty()) {
                auto *blocked = textLabel(QStringLiteral("Prerequisites: %1").arg(goal.blockedBy.join(QStringLiteral(", "))));
                blocked->setObjectName(QStringLiteral("goalBlockedBy"));
                blocked->setStyleSheet(QStringLiteral("color:#9b4845;font-size:10px;"));
                cardLayout->addWidget(blocked);
            } else if (!goal.prerequisiteTitles.isEmpty()) {
                auto *prerequisites = textLabel(QStringLiteral("Prerequisites: %1").arg(goal.prerequisiteTitles.join(QStringLiteral(", "))));
                prerequisites->setStyleSheet(QStringLiteral("color:#55734d;font-size:10px;"));
                cardLayout->addWidget(prerequisites);
            }

            if (goal.goalType == QStringLiteral("checklist")) {
                auto *tasks = new QWidget(card);
                auto *tasksLayout = new QVBoxLayout(tasks);
                tasksLayout->setContentsMargins(3, 1, 3, 1);
                tasksLayout->setSpacing(1);
                for (const GoalTask &task : goal.tasks) {
                    auto *check = new QCheckBox(task.title, tasks);
                    check->setObjectName(QStringLiteral("goalTaskCheckbox"));
                    check->setChecked(task.completed);
                    tasksLayout->addWidget(check);
                    connect(check, &QCheckBox::toggled, this, [this, taskId = task.id](bool checked) {
                        QString taskError;
                        if (!m_repository.setGoalTaskCompleted(taskId, checked, &taskError))
                            QMessageBox::information(this, QStringLiteral("Goal is locked"), taskError);
                        refreshGames(m_selectedGameId);
                    });
                }
                cardLayout->addWidget(tasks);
            } else if (goal.goalType == QStringLiteral("numeric")) {
                auto *numericRow = new QWidget(card);
                auto *numericLayout = new QHBoxLayout(numericRow);
                numericLayout->setContentsMargins(0, 0, 0, 0);
                numericLayout->addWidget(textLabel(QStringLiteral("Current: %1 / %2")
                    .arg(goal.current, 0, 'g', 8).arg(goal.target.toDouble(), 0, 'g', 8)));
                auto *current = new QDoubleSpinBox(numericRow);
                current->setRange(0, 1.0e12);
                current->setDecimals(8);
                current->setValue(goal.current);
                current->setMaximumWidth(150);
                auto *update = button(QStringLiteral("Update progress"), QStringLiteral("softButton"), numericRow);
                numericLayout->addStretch(1);
                numericLayout->addWidget(current);
                numericLayout->addWidget(update);
                cardLayout->addWidget(numericRow);
                connect(update, &QPushButton::clicked, this, [this, id = goal.id, current] {
                    QString updateError;
                    if (!m_repository.setNumericGoalCurrent(id, current->value(), &updateError))
                        QMessageBox::information(this, QStringLiteral("Could not update progress"), updateError);
                    refreshGames(m_selectedGameId);
                });
            } else if (goal.goalType == QStringLiteral("field_target")) {
                QString sourceLabel = QStringLiteral("Linked field");
                const auto linkedObject = std::find_if(m_objects.cbegin(), m_objects.cend(), [&](const ObjectRecord &object) { return object.id == goal.objectId; });
                if (linkedObject != m_objects.cend()) {
                    sourceLabel = linkedObject->name;
                    const auto linkedType = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &type) { return type.id == linkedObject->typeId; });
                    if (linkedType != m_types.cend()) {
                        const auto linkedField = std::find_if(linkedType->fields.cbegin(), linkedType->fields.cend(), [&](const FieldDefinition &field) { return field.id == goal.fieldId; });
                        if (linkedField != linkedType->fields.cend()) sourceLabel += QStringLiteral(" · ") + linkedField->name;
                    }
                }
                auto *current = textLabel(QStringLiteral("%1 · Live value: %2 / %3").arg(sourceLabel)
                    .arg(goal.current, 0, 'g', 8).arg(goal.target.toDouble(), 0, 'g', 8));
                current->setStyleSheet(QStringLiteral("color:#563d66;font-size:11px;font-weight:800;"));
                cardLayout->addWidget(current);
            }

            auto *actions = new QHBoxLayout();
            if (goal.goalType == QStringLiteral("manual")) {
                auto *complete = button(goal.completed ? QStringLiteral("Mark active") : QStringLiteral("Mark complete"), QStringLiteral("softButton"), card);
                actions->addWidget(complete);
                connect(complete, &QPushButton::clicked, this, [this, id = goal.id, completed = goal.completed] {
                    QString goalError;
                    if (!m_repository.setGoalCompleted(id, !completed, &goalError))
                        QMessageBox::information(this, QStringLiteral("Goal is locked"), goalError);
                    refreshGames(m_selectedGameId);
                });
            }
            auto *edit = button(QStringLiteral("✎ Edit"), QStringLiteral("softButton"), card);
            auto *remove = button(QStringLiteral("Delete"), QStringLiteral("softButton"), card);
            actions->addWidget(edit);
            actions->addWidget(remove);
            actions->addStretch(1);
            cardLayout->addLayout(actions);
            connect(edit, &QPushButton::clicked, this, [this, id = goal.id] { openGoalEditor(id); });
            connect(remove, &QPushButton::clicked, this, [this, id = goal.id, titleText = goal.title] {
                if (QMessageBox::question(this, QStringLiteral("Delete goal"),
                    QStringLiteral("Delete %1 and its checklist tasks?").arg(titleText)) != QMessageBox::Yes) return;
                QString deleteError;
                if (!m_repository.deleteGoal(id, &deleteError)) QMessageBox::warning(this, QStringLiteral("Could not delete goal"), deleteError);
                refreshGames(m_selectedGameId);
            });
            list->addWidget(card);
        }
        list->addStretch(1);
        scroll->setWidget(content);
        auto *view = new QWidget(m_pages->widget(2));
        auto *viewLayout = new QVBoxLayout(view);
        viewLayout->setContentsMargins(0, 0, 0, 0);
        viewLayout->addWidget(scroll, 1);
        if (pageCount > 1) {
            auto *navigation = new QHBoxLayout;
            auto *previous = button(QStringLiteral("Previous"), QStringLiteral("goalPreviousPage"), view);
            auto *next = button(QStringLiteral("Next"), QStringLiteral("goalNextPage"), view);
            previous->setEnabled(m_goalPage > 0);
            next->setEnabled(m_goalPage + 1 < pageCount);
            navigation->addWidget(previous);
            navigation->addWidget(textLabel(QStringLiteral("Page %1 of %2").arg(m_goalPage + 1).arg(pageCount)), 1);
            navigation->addWidget(next);
            viewLayout->addLayout(navigation);
            connect(previous, &QPushButton::clicked, this, [this] {
                --m_goalPage;
                refreshGoalPage();
            });
            connect(next, &QPushButton::clicked, this, [this] {
                ++m_goalPage;
                refreshGoalPage();
            });
        }
        m_pageLayouts.at(2)->insertWidget(2, view, 1);
        m_emptyStates.at(2)->hide();
        m_featureViews[2] = view;
    } else {
        auto *state = m_emptyStates.at(2);
        if (auto *title = state->findChild<QLabel *>(QStringLiteral("emptyTitleLabel")))
            title->setText(m_goals.isEmpty() ? QStringLiteral("EVERY QUEST STARTS SOMEWHERE") : QStringLiteral("NO MATCHING GOALS"));
        if (auto *description = state->findChild<QLabel *>(QStringLiteral("emptyDescriptionLabel"))) {
            description->setText(!m_goals.isEmpty() ? QStringLiteral("Try a different search or filter.")
                : m_selectedGameId.isEmpty() ? QStringLiteral("Select a game before adding a goal.")
                : QStringLiteral("Create a manual goal, checklist, numeric target, or live field target."));
        }
        state->show();
    }
}

void MainWindow::refreshObjectPage()
{
    if (m_featureViews.at(1)) {
        delete m_featureViews.at(1);
        m_featureViews[1] = nullptr;
    }
    const auto inSection = [this](const ObjectRecord &object) {
        return m_selectedSectionId.isEmpty() || object.sectionIds.contains(m_selectedSectionId);
    };
    const auto section = std::find_if(m_gameSections.cbegin(), m_gameSections.cend(),
                                      [this](const Section &item) { return item.id == m_selectedSectionId; });
    auto *heading = m_pages->widget(1)->findChild<QFrame *>(QStringLiteral("pageHeading"));
    if (auto *description = heading->findChild<QLabel *>(QStringLiteral("mutedText"))) {
        description->setText(section == m_gameSections.cend() ? QStringLiteral("Browse every active object in this game.")
                                                              : QStringLiteral("Objects placed in %1").arg(section->name));
    }
    if (std::any_of(m_objects.cbegin(), m_objects.cend(), inSection)) {
        auto *scroll = new QScrollArea(m_pages->widget(1));
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(QStringLiteral("QScrollArea { border:0;background:transparent; }"));
        auto *content = new QWidget(scroll);
        auto *grid = new QGridLayout(content);
        grid->setContentsMargins(1, 1, 8, 8);
        grid->setSpacing(10);
        QMap<QString, QString> names;
        for (const ObjectRecord &item : std::as_const(m_objects)) names.insert(item.id, item.name);
        int index = 0;
        for (const ObjectRecord &object : std::as_const(m_objects)) {
            if (!inSection(object)) continue;
            auto *card = panel(QStringLiteral("objectCard"), content);
            card->setStyleSheet(QStringLiteral("QFrame#objectCard { background:#fff2d6;border:3px solid #c6a36c; }"));
            card->setMinimumHeight(150);
            auto *cardLayout = new QHBoxLayout(card);
            cardLayout->setContentsMargins(7, 7, 7, 7);
            cardLayout->setSpacing(8);
            auto *art = panel(QStringLiteral("objectArt"), card);
            art->setFixedWidth(90);
            art->setMinimumHeight(120);
            art->setStyleSheet(QStringLiteral("QFrame#objectArt { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #b68ee0,stop:.55 #674688,stop:1 #392c57);border:3px solid #77568b; }"));
            auto *artLayout = new QVBoxLayout(art);
            auto *mark = textLabel(object.typeName.left(1).toUpper(), QStringLiteral("inkText"));
            mark->setAlignment(Qt::AlignCenter);
            mark->setStyleSheet(QStringLiteral("color:#ffe9b1;font-size:42px;font-weight:700;"));
            const auto typeIt = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &value) { return value.id == object.typeId; });
            QString artReference = object.image;
            if (artReference.isEmpty() && typeIt != m_types.cend()) artReference = typeIt->image;
            if (!artReference.isEmpty()) {
                const QImage artwork = m_repository.loadAssetImage(artReference);
                if (!artwork.isNull()) {
                    mark->setText({});
                    mark->setPixmap(QPixmap::fromImage(artwork).scaled(82, 108, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                }
            }
            artLayout->addWidget(mark);
            cardLayout->addWidget(art);
            auto *details = new QWidget(card);
            auto *detailsLayout = new QVBoxLayout(details);
            detailsLayout->setContentsMargins(0, 0, 0, 0);
            detailsLayout->setSpacing(4);
            auto *title = textLabel(object.name);
            title->setStyleSheet(QStringLiteral("color:#372746;font-size:16px;font-weight:900;"));
            detailsLayout->addWidget(title);
            auto *type = textLabel(object.typeName);
            type->setStyleSheet(QStringLiteral("color:#735d75;font-size:11px;font-weight:800;"));
            detailsLayout->addWidget(type);
            if (!object.description.isEmpty()) {
                auto *description = textLabel(object.description);
                description->setStyleSheet(QStringLiteral("color:#705b70;font-size:11px;"));
                detailsLayout->addWidget(description);
            }
            auto *values = new QGridLayout();
            values->setHorizontalSpacing(10);
            values->setVerticalSpacing(1);
            int row = 0;
            if (typeIt != m_types.cend()) {
                for (const FieldDefinition &field : prioritizedCardFields(*typeIt)) {
                    QString valueText;
                    if (field.fieldType == QStringLiteral("reference") || field.fieldType == QStringLiteral("references")) {
                        QStringList labels;
                        for (const QString &id : object.references.value(field.id)) labels.append(names.value(id, QStringLiteral("Unavailable")));
                        valueText = labels.join(QStringLiteral(", "));
                    } else if (field.fieldType == QStringLiteral("formula") && object.formulaErrors.contains(field.id)) {
                        valueText = QStringLiteral("⚠ %1").arg(object.formulaErrors.value(field.id));
                    } else {
                        const QJsonValue value = object.values.value(field.id);
                        if (field.fieldType == QStringLiteral("progress") && value.isObject()) {
                            valueText = QStringLiteral("%1 / %2").arg(value.toObject().value(QStringLiteral("current")).toVariant().toString(),
                                value.toObject().value(QStringLiteral("max")).toVariant().toString());
                        } else if (value.isBool()) valueText = value.toBool() ? QStringLiteral("Yes") : QStringLiteral("No");
                        else if (value.isObject() || value.isArray()) valueText = QStringLiteral("Saved");
                        else if (!value.isNull() && !value.isUndefined()) valueText = value.toVariant().toString();
                    }
                    if (valueText.isEmpty()) continue;
                    auto *fieldLabel = textLabel(QStringLiteral("✦  ") + field.name);
                    fieldLabel->setStyleSheet(QStringLiteral("color:#675370;font-size:10px;font-weight:800;"));
                    auto *fieldValue = textLabel(valueText);
                    fieldValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    fieldValue->setStyleSheet(QStringLiteral("color:#282036;font-size:10px;font-weight:900;"));
                    values->addWidget(fieldLabel, row, 0);
                    values->addWidget(fieldValue, row, 1);
                    ++row;
                }
            }
            if (row > 0) detailsLayout->addLayout(values);
            auto *actions = new QHBoxLayout();
            auto *edit = button(QStringLiteral("✎  Edit"), QStringLiteral("softButton"), details);
            auto *archive = button(QStringLiteral("Archive"), QStringLiteral("softButton"), details);
            actions->addWidget(edit);
            actions->addWidget(archive);
            auto *sections = button(QStringLiteral("Sections"), QStringLiteral("objectSectionsButton"), details);
            auto *history = button(QStringLiteral("History"), QStringLiteral("objectHistoryButton"), details);
            actions->addWidget(history);
            connect(history, &QPushButton::clicked, this, [this, id = object.id, name = object.name] {
                showObjectHistory(this, m_repository, id, name);
            });
            sections->setEnabled(!m_gameSections.isEmpty());
            actions->addWidget(sections);
            connect(sections, &QPushButton::clicked, this, [this, sections, id = object.id, selected = object.sectionIds] {
                QMenu menu(this);
                for (const Section &section : m_gameSections) {
                    auto *action = menu.addAction(section.name);
                    action->setData(section.id);
                    action->setCheckable(true);
                    action->setChecked(selected.contains(section.id));
                }
                auto *action = menu.exec(sections->mapToGlobal(QPoint(0, sections->height())));
                if (!action) return;
                QString error;
                if (!m_repository.setObjectPlacement(id, action->data().toString(), action->isChecked(), &error)) {
                    QMessageBox::warning(this, QStringLiteral("Could not update placement"), error);
                    return;
                }
                refreshGames(m_selectedGameId);
            });
            actions->addStretch(1);
            detailsLayout->addLayout(actions);
            connect(edit, &QPushButton::clicked, this, [this, id = object.id] { openObjectEditor(id); });
            connect(archive, &QPushButton::clicked, this, [this, id = object.id, name = object.name] {
                if (QMessageBox::question(this, QStringLiteral("Archive object"), QStringLiteral("Archive %1? Its data will remain in the local database.").arg(name)) != QMessageBox::Yes) return;
                QString archiveError;
                if (!m_repository.archiveObject(id, &archiveError)) QMessageBox::warning(this, QStringLiteral("Could not archive object"), archiveError);
                else refreshGames(m_selectedGameId);
            });
            cardLayout->addWidget(details, 1);
            grid->addWidget(card, index / 2, index % 2);
            ++index;
        }
        grid->setRowStretch((index + 1) / 2, 1);
        scroll->setWidget(content);
        m_pageLayouts.at(1)->insertWidget(1, scroll, 1);
        m_emptyStates.at(1)->hide();
        m_featureViews[1] = scroll;
    } else {
        if (auto *label = m_emptyStates.at(1)->findChild<QLabel *>(QStringLiteral("emptyDescriptionLabel"))) {
            label->setText(!m_selectedSectionId.isEmpty()
                ? QStringLiteral("Create an object here, or use Sections on an existing object to place it here.")
                : m_selectedGameId.isEmpty() ? QStringLiteral("Select a game before adding objects.")
                : m_types.isEmpty() ? QStringLiteral("Create an object type first.")
                : QStringLiteral("Add your first object to this game."));
        }
        m_emptyStates.at(1)->show();
    }
}

void MainWindow::selectGame(const QString &gameId)
{
    if (m_selectedGameId != gameId) {
        m_selectedSectionId.clear();
        m_goalPage = 0;
    }
    m_selectedGameId = gameId;
    const Game game = currentGame();
    const bool selected = !game.id.isEmpty();
    m_heroTitle->setText(selected ? game.name.toUpper() : QStringLiteral("GAME TRACKER"));
    m_heroDescription->setText(selected ? game.description : QStringLiteral("Choose a game or create your first one."));
    m_heroSymbol->setPixmap({});
    m_heroSymbol->setText(selected ? game.icon : QStringLiteral("✧"));
    if (selected && !game.coverImage.isEmpty()) {
        const QImage cover = m_repository.loadAssetImage(game.coverImage);
        if (!cover.isNull()) {
            m_heroSymbol->setText({});
            m_heroSymbol->setPixmap(QPixmap::fromImage(cover).scaled(m_heroSymbol->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        }
    }
    m_editGameButton->setVisible(selected);
    m_sections->setEnabled(selected);
    refreshSections();
    refreshFeaturePages();
    selectPage(m_currentPage);
    for (QPushButton *gameButton : m_gameList->findChildren<QPushButton *>(QStringLiteral("gameButton"))) {
        gameButton->setChecked(selected && gameButton->property("gameId").toString() == game.id);
    }
}

void MainWindow::selectPage(Page page)
{
    m_currentPage = page;
    const int index = static_cast<int>(page);
    m_pages->setCurrentIndex(index);
    if (QAbstractButton *tab = m_tabGroup->button(index)) tab->setChecked(true);
}

Game MainWindow::currentGame() const
{
    for (const Game &game : m_games) if (game.id == m_selectedGameId) return game;
    return {};
}

void MainWindow::openCreateGame()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add Game"));
    dialog.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *name = new QLineEdit(&dialog);
    name->setMaxLength(120);
    name->setPlaceholderText(QStringLiteral("For example, Genshin Impact"));
    auto *description = new QTextEdit(&dialog);
    description->setPlaceholderText(QStringLiteral("A short note about this game"));
    description->setMaximumHeight(100);
    auto *icon = new QLineEdit(QStringLiteral("🎮"), &dialog);
    icon->setMaxLength(64);
    auto *coverImage = new QLineEdit(&dialog);
    coverImage->setObjectName(QStringLiteral("gameCoverImageInput"));
    coverImage->setReadOnly(true);
    auto *coverRow = new QWidget(&dialog);
    auto *coverLayout = new QHBoxLayout(coverRow);
    coverLayout->setContentsMargins(0, 0, 0, 0);
    coverLayout->addWidget(coverImage, 1);
    auto *chooseCover = button(QStringLiteral("Choose…"), QStringLiteral("softButton"), coverRow);
    coverLayout->addWidget(chooseCover);
    auto *clearCover = button(QStringLiteral("Clear"), QStringLiteral("softButton"), coverRow);
    coverLayout->addWidget(clearCover);
    connect(clearCover, &QPushButton::clicked, &dialog, [coverImage] { coverImage->clear(); });
    connect(chooseCover, &QPushButton::clicked, &dialog, [this, &dialog, coverImage] {
        const QString reference = openAssetLibrary(&dialog, coverImage->text());
        if (!reference.isNull()) coverImage->setText(reference);
    });
    auto *notes = new QTextEdit(&dialog);
    notes->setObjectName(QStringLiteral("gamePersonalNotesInput"));
    notes->setPlaceholderText(QStringLiteral("Private notes for this playthrough"));
    notes->setMaximumHeight(100);
    form->addRow(QStringLiteral("Game name"), name);
    form->addRow(QStringLiteral("Description"), description);
    form->addRow(QStringLiteral("Icon"), icon);
    form->addRow(QStringLiteral("Cover image"), coverRow);
    form->addRow(QStringLiteral("Personal notes"), notes);
    layout->addLayout(form);
    QPushButton *save = nullptr;
    addDialogButtons(layout, &dialog, &save);
    connect(name, &QLineEdit::textChanged, save, [save](const QString &value) { save->setEnabled(!value.trimmed().isEmpty()); });
    name->setFocus();
    if (dialog.exec() != QDialog::Accepted) return;
    QString id;
    QString error;
    if (!m_repository.createGame(name->text(), description->toPlainText(), icon->text(), &id, &error,
                                 notes->toPlainText(), coverImage->text())) {
        QMessageBox::warning(this, QStringLiteral("Could not add game"), error);
        return;
    }
    refreshGames(id);
}

void MainWindow::openEditGame()
{
    Game game = currentGame();
    if (game.id.isEmpty()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit Game"));
    dialog.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *name = new QLineEdit(game.name, &dialog);
    name->setMaxLength(120);
    auto *description = new QTextEdit(&dialog);
    description->setPlainText(game.description);
    description->setMaximumHeight(100);
    auto *icon = new QLineEdit(game.icon, &dialog);
    icon->setMaxLength(64);
    auto *coverImage = new QLineEdit(game.coverImage, &dialog);
    coverImage->setObjectName(QStringLiteral("gameCoverImageInput"));
    coverImage->setReadOnly(true);
    auto *coverRow = new QWidget(&dialog);
    auto *coverLayout = new QHBoxLayout(coverRow);
    coverLayout->setContentsMargins(0, 0, 0, 0);
    coverLayout->addWidget(coverImage, 1);
    auto *chooseCover = button(QStringLiteral("Choose…"), QStringLiteral("softButton"), coverRow);
    coverLayout->addWidget(chooseCover);
    auto *clearCover = button(QStringLiteral("Clear"), QStringLiteral("softButton"), coverRow);
    coverLayout->addWidget(clearCover);
    connect(clearCover, &QPushButton::clicked, &dialog, [coverImage] { coverImage->clear(); });
    connect(chooseCover, &QPushButton::clicked, &dialog, [this, &dialog, coverImage] {
        const QString reference = openAssetLibrary(&dialog, coverImage->text());
        if (!reference.isNull()) coverImage->setText(reference);
    });
    auto *notes = new QTextEdit(&dialog);
    notes->setObjectName(QStringLiteral("gamePersonalNotesInput"));
    notes->setPlainText(game.personalNotes);
    notes->setMaximumHeight(100);
    form->addRow(QStringLiteral("Game name"), name);
    form->addRow(QStringLiteral("Description"), description);
    form->addRow(QStringLiteral("Icon"), icon);
    form->addRow(QStringLiteral("Cover image"), coverRow);
    form->addRow(QStringLiteral("Personal notes"), notes);
    layout->addLayout(form);
    auto *archive = button(QStringLiteral("Archive Game"), QStringLiteral("softButton"), &dialog);
    archive->setObjectName(QStringLiteral("archiveGameButton"));
    layout->addWidget(archive);
    bool archiveRequested = false;
    connect(archive, &QPushButton::clicked, &dialog, [this, &dialog, &archiveRequested, game] {
        if (QMessageBox::question(&dialog, QStringLiteral("Archive game"),
            QStringLiteral("Archive %1? You can restore it from Profile & Settings.").arg(game.name)) != QMessageBox::Yes) return;
        QString error;
        if (!m_repository.archiveGame(game.id, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not archive game"), error);
            return;
        }
        archiveRequested = true;
        dialog.accept();
    });
    QPushButton *save = nullptr;
    addDialogButtons(layout, &dialog, &save);
    connect(name, &QLineEdit::textChanged, save, [save](const QString &value) { save->setEnabled(!value.trimmed().isEmpty()); });
    if (dialog.exec() != QDialog::Accepted) return;
    if (archiveRequested) {
        refreshGames();
        return;
    }
    game.name = name->text();
    game.description = description->toPlainText();
    game.icon = icon->text();
    game.coverImage = coverImage->text();
    game.personalNotes = notes->toPlainText();
    QString error;
    if (!m_repository.updateGame(game, &error)) {
        QMessageBox::warning(this, QStringLiteral("Could not update game"), error);
        return;
    }
    refreshGames(game.id);
}

void MainWindow::openCreateSection()
{
    if (m_selectedGameId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Choose a game"), QStringLiteral("Select a game before adding a section."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add Section"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *name = new QLineEdit(&dialog);
    name->setMaxLength(80);
    name->setPlaceholderText(QStringLiteral("Section name"));
    auto *icon = new QLineEdit(QStringLiteral("✦"), &dialog);
    icon->setMaxLength(16);
    auto *parent = new QComboBox(&dialog);
    parent->addItem(QStringLiteral("Root section"), QString());
    QString error;
    const QVector<Section> sections = m_repository.sections(m_selectedGameId, &error);
    for (const Section &section : sections) parent->addItem(section.name, section.id);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Icon"), icon);
    form->addRow(QStringLiteral("Parent"), parent);
    layout->addLayout(form);
    QPushButton *save = nullptr;
    addDialogButtons(layout, &dialog, &save);
    connect(name, &QLineEdit::textChanged, save, [save](const QString &value) { save->setEnabled(!value.trimmed().isEmpty()); });
    name->setFocus();
    if (dialog.exec() != QDialog::Accepted) return;
    if (!m_repository.createSection(m_selectedGameId, parent->currentData().toString(), name->text(), icon->text(), &error)) {
        QMessageBox::warning(this, QStringLiteral("Could not add section"), error);
        return;
    }
    refreshGames(m_selectedGameId);
}

void MainWindow::openCreateType()
{
    if (m_selectedGameId.isEmpty()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New Object Type"));
    dialog.setMinimumWidth(470);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *name = new QLineEdit(&dialog);
    name->setMaxLength(80);
    auto *description = new QLineEdit(&dialog);
    auto *icon = new QLineEdit(QStringLiteral("◈"), &dialog);
    icon->setMaxLength(64);
    auto *image = new QLineEdit(&dialog);
    image->setObjectName(QStringLiteral("typeFallbackImageInput"));
    image->setReadOnly(true);
    auto *imageRow = new QWidget(&dialog);
    auto *imageLayout = new QHBoxLayout(imageRow);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    imageLayout->addWidget(image, 1);
    auto *chooseImage = button(QStringLiteral("Choose…"), QStringLiteral("softButton"), imageRow);
    imageLayout->addWidget(chooseImage);
    auto *clearImage = button(QStringLiteral("Clear"), QStringLiteral("softButton"), imageRow);
    imageLayout->addWidget(clearImage);
    connect(clearImage, &QPushButton::clicked, &dialog, [image] { image->clear(); });
    connect(chooseImage, &QPushButton::clicked, &dialog, [this, &dialog, image] {
        const QString reference = openAssetLibrary(&dialog, image->text());
        if (!reference.isNull()) image->setText(reference);
    });
    form->addRow(QStringLiteral("Type name"), name);
    form->addRow(QStringLiteral("Description"), description);
    form->addRow(QStringLiteral("Icon"), icon);
    form->addRow(QStringLiteral("Fallback image"), imageRow);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *save = buttons->addButton(QStringLiteral("Create Type"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("goldButton"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [this, &dialog, name, description, icon, image] {
        if (name->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Type name required"), QStringLiteral("Enter a name for this object type."));
            name->setFocus();
            return;
        }
        QString error;
        if (!m_repository.createObjectType(m_selectedGameId, name->text(), description->text(), icon->text(), nullptr, &error, image->text())) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not create type"), error);
            return;
        }
        dialog.accept();
    });
    name->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        selectPage(Page::Types);
        refreshGames(m_selectedGameId);
    }
}

void MainWindow::openEditType(const QString &typeId)
{
    const auto typeIt = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &type) { return type.id == typeId; });
    if (typeIt == m_types.cend()) return;
    const ObjectType original = *typeIt;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit Object Type · %1").arg(original.name));
    dialog.setMinimumWidth(470);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *name = new QLineEdit(original.name, &dialog);
    name->setObjectName(QStringLiteral("typeNameInput"));
    name->setMaxLength(80);
    auto *description = new QLineEdit(original.description, &dialog);
    description->setObjectName(QStringLiteral("typeDescriptionInput"));
    auto *icon = new QLineEdit(original.icon, &dialog);
    icon->setObjectName(QStringLiteral("typeIconInput"));
    icon->setMaxLength(64);
    auto *image = new QLineEdit(original.image, &dialog);
    image->setReadOnly(true);
    auto *imageRow = new QWidget(&dialog);
    auto *imageLayout = new QHBoxLayout(imageRow);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    imageLayout->addWidget(image, 1);
    auto *chooseImage = button(QStringLiteral("Choose…"), QStringLiteral("softButton"), imageRow);
    imageLayout->addWidget(chooseImage);
    auto *clearImage = button(QStringLiteral("Clear"), QStringLiteral("softButton"), imageRow);
    imageLayout->addWidget(clearImage);
    connect(clearImage, &QPushButton::clicked, &dialog, [image] { image->clear(); });
    connect(chooseImage, &QPushButton::clicked, &dialog, [this, &dialog, image] {
        const QString reference = openAssetLibrary(&dialog, image->text());
        if (!reference.isNull()) image->setText(reference);
    });
    form->addRow(QStringLiteral("Type name"), name);
    form->addRow(QStringLiteral("Description"), description);
    form->addRow(QStringLiteral("Icon"), icon);
    form->addRow(QStringLiteral("Fallback image"), imageRow);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *save = buttons->addButton(QStringLiteral("Save Type"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("goldButton"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [this, &dialog, original, name, description, icon, image] {
        ObjectType edited = original;
        edited.name = name->text();
        edited.description = description->text();
        edited.icon = icon->text();
        edited.image = image->text();
        QString error;
        if (!m_repository.updateObjectType(edited, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not update type"), error);
            return;
        }
        dialog.accept();
    });
    name->setFocus();
    if (dialog.exec() == QDialog::Accepted) refreshGames(m_selectedGameId);
}

void MainWindow::openCreateField(const QString &typeId)
{
    const auto typeIt = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &type) { return type.id == typeId; });
    if (typeIt == m_types.cend()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add Field · %1").arg(typeIt->name));
    dialog.setMinimumWidth(500);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *name = new QLineEdit(&dialog);
    name->setObjectName(QStringLiteral("fieldNameInput"));
    name->setMaxLength(80);
    auto *kind = new QComboBox(&dialog);
    kind->setObjectName(QStringLiteral("fieldTypeChoice"));
    const QList<QPair<QString, QString>> fieldTypes = {
        {QStringLiteral("Text"), QStringLiteral("text")}, {QStringLiteral("Long text"), QStringLiteral("long_text")},
        {QStringLiteral("Integer"), QStringLiteral("integer")}, {QStringLiteral("Decimal"), QStringLiteral("decimal")},
        {QStringLiteral("Percentage"), QStringLiteral("percentage")}, {QStringLiteral("Boolean"), QStringLiteral("boolean")},
        {QStringLiteral("Select"), QStringLiteral("select")}, {QStringLiteral("Date"), QStringLiteral("date")},
        {QStringLiteral("Progress"), QStringLiteral("progress")}, {QStringLiteral("Reference"), QStringLiteral("reference")},
        {QStringLiteral("Multiple references"), QStringLiteral("references")}, {QStringLiteral("Formula"), QStringLiteral("formula")}
    };
    for (const auto &entry : fieldTypes) kind->addItem(entry.first, entry.second);
    auto *icon = new QLineEdit(QStringLiteral("✦"), &dialog);
    icon->setMaxLength(64);
    auto *description = new QLineEdit(&dialog);
    description->setObjectName(QStringLiteral("fieldDescriptionInput"));
    description->setPlaceholderText(QStringLiteral("Help text shown with this field"));
    form->addRow(QStringLiteral("Field name"), name);
    form->addRow(QStringLiteral("Field type"), kind);
    form->addRow(QStringLiteral("Icon"), icon);
    form->addRow(QStringLiteral("Description"), description);
    auto *defaultEnabled = new QCheckBox(QStringLiteral("Set default"), &dialog);
    defaultEnabled->setObjectName(QStringLiteral("fieldDefaultEnabled"));
    auto *defaultValues = new QStackedWidget(&dialog);
    auto *scalarDefault = new QLineEdit(defaultValues);
    scalarDefault->setObjectName(QStringLiteral("fieldDefaultScalar"));
    scalarDefault->setMaxLength(50000);
    scalarDefault->setPlaceholderText(QStringLiteral("Default value"));
    defaultValues->addWidget(scalarDefault);
    auto *booleanDefault = new QComboBox(defaultValues);
    booleanDefault->setObjectName(QStringLiteral("fieldDefaultBoolean"));
    booleanDefault->addItem(QStringLiteral("True"), true);
    booleanDefault->addItem(QStringLiteral("False"), false);
    defaultValues->addWidget(booleanDefault);
    auto *progressDefault = new QWidget(defaultValues);
    auto *progressDefaultLayout = new QHBoxLayout(progressDefault);
    progressDefaultLayout->setContentsMargins(0, 0, 0, 0);
    auto *defaultCurrent = new QLineEdit(progressDefault);
    defaultCurrent->setObjectName(QStringLiteral("fieldDefaultProgressCurrent"));
    defaultCurrent->setValidator(new QDoubleValidator(defaultCurrent));
    defaultCurrent->setPlaceholderText(QStringLiteral("Current"));
    auto *defaultMaximum = new QLineEdit(progressDefault);
    defaultMaximum->setObjectName(QStringLiteral("fieldDefaultProgressMaximum"));
    defaultMaximum->setValidator(new QDoubleValidator(defaultMaximum));
    defaultMaximum->setPlaceholderText(QStringLiteral("Maximum"));
    progressDefaultLayout->addWidget(defaultCurrent);
    progressDefaultLayout->addWidget(defaultMaximum);
    defaultValues->addWidget(progressDefault);
    auto *defaultRow = new QWidget(&dialog);
    defaultRow->setObjectName(QStringLiteral("fieldDefaultRow"));
    auto *defaultRowLayout = new QHBoxLayout(defaultRow);
    defaultRowLayout->setContentsMargins(0, 0, 0, 0);
    defaultRowLayout->addWidget(defaultEnabled);
    defaultRowLayout->addWidget(defaultValues, 1);
    defaultValues->setEnabled(false);
    connect(defaultEnabled, &QCheckBox::toggled, defaultValues, &QWidget::setEnabled);
    form->addRow(QStringLiteral("Default"), defaultRow);
    auto *choicesLabel = textLabel(QStringLiteral("Choices (one per line)"));
    auto *choices = new QTextEdit(&dialog);
    choices->setPlaceholderText(QStringLiteral("Common\nUncommon\nRare"));
    choices->setMaximumHeight(100);
    form->addRow(choicesLabel, choices);
    auto *targetType = new QComboBox(&dialog);
    targetType->addItem(QStringLiteral("Any type in this game"), QString());
    for (const ObjectType &target : std::as_const(m_types)) targetType->addItem(target.name, target.id);
    form->addRow(QStringLiteral("Allowed reference type"), targetType);
    auto *formulaLabel = textLabel(QStringLiteral("Expression (use @{field-id} to refer to a field)"), {}, &dialog);
    auto *formulaExpression = new QTextEdit(&dialog);
    formulaExpression->setObjectName(QStringLiteral("formulaExpression"));
    formulaExpression->setPlaceholderText(QStringLiteral("(@{field-id} + 2) * 3"));
    formulaExpression->setMaximumHeight(82);
    form->addRow(formulaLabel, formulaExpression);
    auto *formulaInsert = new QComboBox(&dialog);
    formulaInsert->setObjectName(QStringLiteral("formulaInsertField"));
    formulaInsert->addItem(QStringLiteral("Insert numeric field…"), QString());
    for (const FieldDefinition &field : typeIt->fields) {
        if (!field.archived && (field.fieldType == QStringLiteral("integer") || field.fieldType == QStringLiteral("decimal") ||
            field.fieldType == QStringLiteral("percentage") || field.fieldType == QStringLiteral("progress") || field.fieldType == QStringLiteral("formula")))
            formulaInsert->addItem(QStringLiteral("%1 · %2").arg(field.name, field.id), field.id);
    }
    form->addRow(QStringLiteral("Insert field reference"), formulaInsert);
    connect(formulaInsert, &QComboBox::currentIndexChanged, &dialog, [formulaInsert, formulaExpression](int index) {
        const QString id = formulaInsert->itemData(index).toString();
        if (id.isEmpty()) return;
        QTextCursor cursor = formulaExpression->textCursor();
        cursor.insertText(QStringLiteral("@{%1}").arg(id));
        formulaExpression->setTextCursor(cursor);
        formulaInsert->setCurrentIndex(0);
    });

    auto *aggregationRelation = new QComboBox(&dialog);
    aggregationRelation->setObjectName(QStringLiteral("formulaAggregationRelation"));
    aggregationRelation->addItem(QStringLiteral("No relationship aggregation"), QString());
    for (const FieldDefinition &field : typeIt->fields) {
        if (!field.archived && (field.fieldType == QStringLiteral("reference") || field.fieldType == QStringLiteral("references")))
            aggregationRelation->addItem(QStringLiteral("%1 · %2").arg(field.name, field.id), field.id);
    }
    form->addRow(QStringLiteral("Aggregate related objects"), aggregationRelation);
    auto *aggregationOperation = new QComboBox(&dialog);
    aggregationOperation->setObjectName(QStringLiteral("formulaAggregationOperation"));
    for (const auto &entry : QList<QPair<QString, QString>>{
             {QStringLiteral("Sum"), QStringLiteral("sum")}, {QStringLiteral("Average"), QStringLiteral("avg")},
             {QStringLiteral("Minimum"), QStringLiteral("min")}, {QStringLiteral("Maximum"), QStringLiteral("max")},
             {QStringLiteral("Count"), QStringLiteral("count")}})
        aggregationOperation->addItem(entry.first, entry.second);
    form->addRow(QStringLiteral("Operation"), aggregationOperation);
    auto *aggregationField = new QComboBox(&dialog);
    aggregationField->setObjectName(QStringLiteral("formulaAggregationField"));
    aggregationField->addItem(QStringLiteral("Choose numeric target field…"), QString());
    for (const ObjectType &target : std::as_const(m_types)) {
        for (const FieldDefinition &field : target.fields) {
            if (!field.archived && (field.fieldType == QStringLiteral("integer") || field.fieldType == QStringLiteral("decimal") ||
                field.fieldType == QStringLiteral("percentage") || field.fieldType == QStringLiteral("progress")))
                aggregationField->addItem(QStringLiteral("%1 · %2").arg(target.name, field.name), field.id);
        }
    }
    form->addRow(QStringLiteral("Target numeric field"), aggregationField);
    auto *sampleObject = new QComboBox(&dialog);
    sampleObject->setObjectName(QStringLiteral("formulaPreviewObject"));
    sampleObject->addItem(QStringLiteral("Choose an example object…"), QString());
    for (const ObjectRecord &object : m_objects) if (object.typeId == typeId)
        sampleObject->addItem(object.name, object.id);
    form->addRow(QStringLiteral("Preview with object"), sampleObject);
    auto *formulaPreview = textLabel(QStringLiteral("Enter an expression and choose an example object for a live preview."), {}, &dialog);
    formulaPreview->setObjectName(QStringLiteral("formulaPreview"));
    formulaPreview->setStyleSheet(QStringLiteral("color:#715c74;font-size:11px;"));
    form->addRow(QStringLiteral("Preview"), formulaPreview);
    auto refreshAggregationFields = [this, typeId, aggregationRelation, aggregationOperation, aggregationField] {
        const QString relationId = aggregationRelation->currentData().toString();
        const QString selectedId = aggregationField->currentData().toString();
        QString allowedType;
        const auto sourceType = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &type) { return type.id == typeId; });
        if (sourceType != m_types.cend()) {
            const auto relation = std::find_if(sourceType->fields.cbegin(), sourceType->fields.cend(), [&](const FieldDefinition &field) { return field.id == relationId; });
            if (relation != sourceType->fields.cend()) allowedType = relation->options.value(QStringLiteral("allowed_type_id")).toString();
        }
        aggregationField->clear();
        aggregationField->addItem(QStringLiteral("Choose numeric target field…"), QString());
        for (const ObjectType &target : std::as_const(m_types)) {
            if (!allowedType.isEmpty() && target.id != allowedType) continue;
            for (const FieldDefinition &field : target.fields) {
                if (!field.archived && (field.fieldType == QStringLiteral("integer") || field.fieldType == QStringLiteral("decimal") ||
                    field.fieldType == QStringLiteral("percentage") || field.fieldType == QStringLiteral("progress")))
                    aggregationField->addItem(QStringLiteral("%1 · %2").arg(target.name, field.name), field.id);
            }
        }
        const int selectedIndex = aggregationField->findData(selectedId);
        if (selectedIndex >= 0) aggregationField->setCurrentIndex(selectedIndex);
        const bool aggregationEnabled = !relationId.isEmpty();
        aggregationOperation->setVisible(aggregationEnabled);
        aggregationField->setVisible(aggregationEnabled && aggregationOperation->currentData().toString() != QStringLiteral("count"));
    };
    connect(aggregationRelation, &QComboBox::currentIndexChanged, &dialog, refreshAggregationFields);
    connect(aggregationOperation, &QComboBox::currentIndexChanged, &dialog, [refreshAggregationFields] { refreshAggregationFields(); });
    auto updateFormulaPreview = [this, typeId, formulaExpression, aggregationRelation, aggregationOperation,
                                 aggregationField, sampleObject, formulaPreview] {
        const QString expression = formulaExpression->toPlainText();
        QString syntaxError;
        if (!FormulaEvaluator::validateExpression(expression, &syntaxError)) {
            formulaPreview->setText(syntaxError.isEmpty() ? QStringLiteral("Formula preview is waiting for a valid expression.") : syntaxError);
            return;
        }
        const QString objectId = sampleObject->currentData().toString();
        if (objectId.isEmpty()) {
            formulaPreview->setText(QStringLiteral("Choose an example object to calculate this expression."));
            return;
        }
        QJsonObject options{{QStringLiteral("expression"), expression}};
        if (!aggregationRelation->currentData().toString().isEmpty()) {
            QJsonObject aggregation{{QStringLiteral("relationship_field_id"), aggregationRelation->currentData().toString()},
                                    {QStringLiteral("operation"), aggregationOperation->currentData().toString()}};
            if (aggregationOperation->currentData().toString() != QStringLiteral("count"))
                aggregation.insert(QStringLiteral("field_id"), aggregationField->currentData().toString());
            options.insert(QStringLiteral("aggregation"), aggregation);
        }
        QVector<ObjectRecord> objects = m_repository.objects(m_selectedGameId);
        const auto found = std::find_if(objects.begin(), objects.end(), [&](const ObjectRecord &item) { return item.id == objectId; });
        if (found == objects.end()) {
            formulaPreview->setText(QStringLiteral("The example object is no longer available. Refresh and try again."));
            return;
        }
        QVector<ObjectType> types = m_repository.objectTypes(m_selectedGameId);
        auto type = std::find_if(types.begin(), types.end(), [&](const ObjectType &item) { return item.id == typeId; });
        if (type == types.end()) return;
        FieldDefinition candidate;
        candidate.id = QStringLiteral("__preview_formula__");
        candidate.objectTypeId = typeId;
        candidate.name = QStringLiteral("Formula preview");
        candidate.fieldType = QStringLiteral("formula");
        candidate.options = options;
        type->fields.append(candidate);
        found->formulaErrors.remove(candidate.id);
        FormulaEvaluator::evaluate(type->fields, *found, types, objects);
        const auto failure = found->formulaErrors.constFind(candidate.id);
        if (failure != found->formulaErrors.cend()) formulaPreview->setText(QStringLiteral("Preview error: %1").arg(failure.value()));
        else formulaPreview->setText(QStringLiteral("Result: %1").arg(found->values.value(candidate.id).toDouble(), 0, 'g', 12));
    };
    connect(formulaExpression, &QTextEdit::textChanged, &dialog, updateFormulaPreview);
    connect(aggregationRelation, &QComboBox::currentIndexChanged, &dialog, updateFormulaPreview);
    connect(aggregationOperation, &QComboBox::currentIndexChanged, &dialog, updateFormulaPreview);
    connect(aggregationField, &QComboBox::currentIndexChanged, &dialog, updateFormulaPreview);
    connect(sampleObject, &QComboBox::currentIndexChanged, &dialog, updateFormulaPreview);

    auto updateOptions = [kind, choicesLabel, choices, targetType, formulaLabel, formulaExpression,
                          formulaInsert, aggregationRelation, aggregationOperation, aggregationField, sampleObject, formulaPreview] {
        const QString selected = kind->currentData().toString();
        const bool select = selected == QStringLiteral("select");
        const bool references = selected == QStringLiteral("reference") || selected == QStringLiteral("references");
        const bool formula = selected == QStringLiteral("formula");
        choicesLabel->setVisible(select);
        choices->setVisible(select);
        targetType->setVisible(references);
        const QList<QWidget *> formulaWidgets{formulaLabel, formulaExpression, formulaInsert, aggregationRelation,
            aggregationOperation, aggregationField, sampleObject, formulaPreview};
        for (QWidget *widget : formulaWidgets) widget->setVisible(formula);
        aggregationOperation->setVisible(formula && !aggregationRelation->currentData().toString().isEmpty());
        aggregationField->setVisible(formula && !aggregationRelation->currentData().toString().isEmpty() &&
            aggregationOperation->currentData().toString() != QStringLiteral("count"));
    };
    auto updateDefaultInput = [kind, defaultRow, defaultValues, defaultEnabled, scalarDefault, defaultCurrent, defaultMaximum] {
        const QString fieldType = kind->currentData().toString();
        const bool unsupported = fieldType == QStringLiteral("formula") || fieldType == QStringLiteral("reference") || fieldType == QStringLiteral("references");
        defaultRow->setVisible(!unsupported);
        if (unsupported) defaultEnabled->setChecked(false);
        if (fieldType == QStringLiteral("boolean")) defaultValues->setCurrentIndex(1);
        else if (fieldType == QStringLiteral("progress")) defaultValues->setCurrentIndex(2);
        else defaultValues->setCurrentIndex(0);
        scalarDefault->setValidator(nullptr);
        if (fieldType == QStringLiteral("integer")) scalarDefault->setValidator(new QIntValidator(scalarDefault));
        else if (fieldType == QStringLiteral("percentage")) scalarDefault->setValidator(new QDoubleValidator(0, 100, 8, scalarDefault));
        else if (fieldType == QStringLiteral("decimal")) scalarDefault->setValidator(new QDoubleValidator(scalarDefault));
        scalarDefault->setPlaceholderText(fieldType == QStringLiteral("date") ? QStringLiteral("YYYY-MM-DD")
            : fieldType == QStringLiteral("select") ? QStringLiteral("One of the choices above")
            : QStringLiteral("Default value"));
        Q_UNUSED(defaultCurrent);
        Q_UNUSED(defaultMaximum);
    };
    connect(kind, &QComboBox::currentIndexChanged, &dialog, [updateOptions, updateDefaultInput] {
        updateOptions();
        updateDefaultInput();
    });
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *save = buttons->addButton(QStringLiteral("Add Field"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("goldButton"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [this, &dialog, typeId, name, kind, icon, description, choices, targetType,
                                                   defaultEnabled, scalarDefault, booleanDefault, defaultCurrent, defaultMaximum,
                                                   formulaExpression, aggregationRelation, aggregationOperation, aggregationField] {
        if (name->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Field name required"), QStringLiteral("Enter a name for this field."));
            name->setFocus();
            return;
        }
        QJsonObject options;
        const QString fieldType = kind->currentData().toString();
        if (fieldType == QStringLiteral("select")) {
            QJsonArray choiceValues;
            QSet<QString> unique;
            for (const QString &line : choices->toPlainText().split(QLatin1Char('\n'))) {
                const QString choice = line.trimmed();
                if (choice.isEmpty()) continue;
                if (unique.contains(choice)) {
                    QMessageBox::warning(&dialog, QStringLiteral("Duplicate choice"), QStringLiteral("Remove duplicate choices before continuing."));
                    return;
                }
                if (choice.size() > 100 || choiceValues.size() >= 200) {
                    QMessageBox::warning(&dialog, QStringLiteral("Too many or too long choices"), QStringLiteral("Each choice must be 100 characters or fewer, with at most 200 choices."));
                    return;
                }
                unique.insert(choice);
                choiceValues.append(choice);
            }
            if (choiceValues.isEmpty()) {
                QMessageBox::warning(&dialog, QStringLiteral("Choices required"), QStringLiteral("Add at least one select choice."));
                return;
            }
            options.insert(QStringLiteral("choices"), choiceValues);
        }
        if (fieldType == QStringLiteral("reference") || fieldType == QStringLiteral("references")) {
            const QString allowed = targetType->currentData().toString();
            if (!allowed.isEmpty()) options.insert(QStringLiteral("allowed_type_id"), allowed);
        }
        if (fieldType == QStringLiteral("formula")) {
            options.insert(QStringLiteral("expression"), formulaExpression->toPlainText());
            const QString relationshipId = aggregationRelation->currentData().toString();
            if (!relationshipId.isEmpty()) {
                QJsonObject aggregation{{QStringLiteral("relationship_field_id"), relationshipId},
                                        {QStringLiteral("operation"), aggregationOperation->currentData().toString()}};
                if (aggregationOperation->currentData().toString() != QStringLiteral("count")) {
                    const QString fieldId = aggregationField->currentData().toString();
                    if (fieldId.isEmpty()) {
                        QMessageBox::warning(&dialog, QStringLiteral("Target field required"), QStringLiteral("Choose a numeric target field for this aggregation."));
                        return;
                    }
                    aggregation.insert(QStringLiteral("field_id"), fieldId);
                }
                options.insert(QStringLiteral("aggregation"), aggregation);
            }
        }
        QJsonValue defaultValue;
        if (defaultEnabled->isChecked()) {
            if (fieldType == QStringLiteral("boolean")) {
                defaultValue = QJsonValue(booleanDefault->currentData().toBool());
            } else if (fieldType == QStringLiteral("progress")) {
                bool currentOkay = false;
                bool maximumOkay = false;
                const double current = defaultCurrent->text().trimmed().toDouble(&currentOkay);
                const double maximum = defaultMaximum->text().trimmed().toDouble(&maximumOkay);
                if (!currentOkay || !maximumOkay) {
                    QMessageBox::warning(&dialog, QStringLiteral("Invalid progress default"), QStringLiteral("Enter both a current value and a maximum."));
                    return;
                }
                defaultValue = QJsonObject{{QStringLiteral("current"), current}, {QStringLiteral("max"), maximum}};
            } else if (fieldType == QStringLiteral("integer") || fieldType == QStringLiteral("decimal") ||
                       fieldType == QStringLiteral("percentage")) {
                bool okay = false;
                const double value = scalarDefault->text().trimmed().toDouble(&okay);
                if (!okay || (fieldType == QStringLiteral("integer") && std::floor(value) != value)) {
                    QMessageBox::warning(&dialog, QStringLiteral("Invalid default"), QStringLiteral("Enter a valid numeric default for this field."));
                    return;
                }
                defaultValue = value;
            } else {
                defaultValue = scalarDefault->text();
            }
        }
        QString error;
        if (!m_repository.createField(typeId, name->text(), fieldType, icon->text(), options, nullptr, &error,
                                      description->text(), defaultValue)) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not add field"), error);
            return;
        }
        dialog.accept();
    });
    updateOptions();
    updateDefaultInput();
    refreshAggregationFields();
    updateFormulaPreview();
    name->setFocus();
    if (dialog.exec() == QDialog::Accepted) refreshGames(m_selectedGameId);
}

void MainWindow::openEditField(const QString &typeId, const QString &fieldId)
{
    const auto typeIt = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &type) { return type.id == typeId; });
    if (typeIt == m_types.cend()) return;
    const auto fieldIt = std::find_if(typeIt->fields.cbegin(), typeIt->fields.cend(), [&](const FieldDefinition &field) { return field.id == fieldId; });
    if (fieldIt == typeIt->fields.cend() || fieldIt->archived) return;
    const ObjectType originalType = *typeIt;
    const FieldDefinition original = *fieldIt;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit Field · %1").arg(original.name));
    dialog.setMinimumWidth(560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *name = new QLineEdit(original.name, &dialog);
    name->setObjectName(QStringLiteral("fieldEditNameInput"));
    name->setMaxLength(80);
    auto *icon = new QLineEdit(original.icon, &dialog);
    icon->setObjectName(QStringLiteral("fieldEditIconInput"));
    icon->setMaxLength(64);
    auto *description = new QLineEdit(original.description, &dialog);
    description->setObjectName(QStringLiteral("fieldEditDescriptionInput"));
    auto *sortOrder = new QSpinBox(&dialog);
    sortOrder->setObjectName(QStringLiteral("fieldEditSortOrder"));
    sortOrder->setRange(0, 1000000);
    sortOrder->setValue(std::max(0, original.sortOrder));
    form->addRow(QStringLiteral("Field name"), name);
    form->addRow(QStringLiteral("Icon"), icon);
    form->addRow(QStringLiteral("Description"), description);
    form->addRow(QStringLiteral("Order"), sortOrder);

    auto *defaultValue = new QLineEdit(&dialog);
    defaultValue->setObjectName(QStringLiteral("fieldEditDefaultValue"));
    if (original.defaultValue.isString()) defaultValue->setText(original.defaultValue.toString());
    else if (!original.defaultValue.isUndefined() && !original.defaultValue.isNull()) {
        if (original.defaultValue.isObject()) defaultValue->setText(QString::fromUtf8(QJsonDocument(original.defaultValue.toObject()).toJson(QJsonDocument::Compact)));
        else if (original.defaultValue.isArray()) defaultValue->setText(QString::fromUtf8(QJsonDocument(original.defaultValue.toArray()).toJson(QJsonDocument::Compact)));
        else defaultValue->setText(QString::fromUtf8(QJsonDocument(QJsonArray{original.defaultValue}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
    }
    defaultValue->setPlaceholderText(QStringLiteral("Leave empty to remove the default"));
    const bool supportsDefault = original.fieldType != QStringLiteral("formula") && original.fieldType != QStringLiteral("reference") &&
        original.fieldType != QStringLiteral("references");
    defaultValue->setVisible(supportsDefault);
    if (supportsDefault) form->addRow(QStringLiteral("Default value"), defaultValue);

    auto *choices = new QTextEdit(&dialog);
    choices->setObjectName(QStringLiteral("fieldEditChoices"));
    choices->setMaximumHeight(110);
    const QJsonArray previousChoices = original.options.value(QStringLiteral("choices")).toArray();
    QStringList previousChoiceList;
    for (const QJsonValue &choice : previousChoices) previousChoiceList.append(choice.toString());
    choices->setPlainText(previousChoiceList.join(QLatin1Char('\n')));
    choices->setVisible(original.fieldType == QStringLiteral("select"));
    if (original.fieldType == QStringLiteral("select")) form->addRow(QStringLiteral("Choices (one per line)"), choices);

    auto *targetType = new QComboBox(&dialog);
    targetType->setObjectName(QStringLiteral("fieldEditTargetType"));
    targetType->addItem(QStringLiteral("Any type in this game"), QString());
    for (const ObjectType &target : std::as_const(m_types)) targetType->addItem(target.name, target.id);
    const int previousTarget = targetType->findData(original.options.value(QStringLiteral("allowed_type_id")).toString());
    if (previousTarget >= 0) targetType->setCurrentIndex(previousTarget);
    const bool hasReferenceOptions = original.fieldType == QStringLiteral("reference") || original.fieldType == QStringLiteral("references");
    targetType->setVisible(hasReferenceOptions);
    if (hasReferenceOptions) form->addRow(QStringLiteral("Allowed reference type"), targetType);

    auto *formulaExpression = new QTextEdit(&dialog);
    formulaExpression->setObjectName(QStringLiteral("fieldEditFormulaExpression"));
    formulaExpression->setMaximumHeight(90);
    formulaExpression->setPlaceholderText(QStringLiteral("(@{field-id} + 2) * 3"));
    formulaExpression->setPlainText(original.options.value(QStringLiteral("expression")).toString());
    const bool formulaField = original.fieldType == QStringLiteral("formula");
    formulaExpression->setVisible(formulaField);
    if (formulaField) form->addRow(QStringLiteral("Formula expression"), formulaExpression);
    auto *insertReference = new QComboBox(&dialog);
    insertReference->addItem(QStringLiteral("Insert numeric field…"), QString());
    for (const FieldDefinition &candidate : originalType.fields) {
        if (candidate.archived || candidate.id == original.id) continue;
        if (candidate.fieldType == QStringLiteral("integer") || candidate.fieldType == QStringLiteral("decimal") ||
            candidate.fieldType == QStringLiteral("percentage") || candidate.fieldType == QStringLiteral("progress") ||
            candidate.fieldType == QStringLiteral("formula"))
            insertReference->addItem(candidate.name, candidate.id);
    }
    insertReference->setVisible(formulaField);
    if (formulaField) form->addRow(QStringLiteral("Insert reference"), insertReference);
    connect(insertReference, &QComboBox::activated, &dialog, [insertReference, formulaExpression](int index) {
        const QString fieldId = insertReference->itemData(index).toString();
        if (fieldId.isEmpty()) return;
        formulaExpression->insertPlainText(QStringLiteral("@{%1}").arg(fieldId));
        insertReference->setCurrentIndex(0);
    });

    auto *aggregationRelation = new QComboBox(&dialog);
    aggregationRelation->addItem(QStringLiteral("No relationship aggregation"), QString());
    for (const FieldDefinition &candidate : originalType.fields) {
        if (!candidate.archived && (candidate.fieldType == QStringLiteral("reference") || candidate.fieldType == QStringLiteral("references")))
            aggregationRelation->addItem(candidate.name, candidate.id);
    }
    auto *aggregationOperation = new QComboBox(&dialog);
    aggregationOperation->addItem(QStringLiteral("Sum"), QStringLiteral("sum"));
    aggregationOperation->addItem(QStringLiteral("Average"), QStringLiteral("avg"));
    aggregationOperation->addItem(QStringLiteral("Minimum"), QStringLiteral("min"));
    aggregationOperation->addItem(QStringLiteral("Maximum"), QStringLiteral("max"));
    aggregationOperation->addItem(QStringLiteral("Count"), QStringLiteral("count"));
    auto *aggregationField = new QComboBox(&dialog);
    const QJsonObject oldAggregation = original.options.value(QStringLiteral("aggregation")).toObject();
    int relationIndex = aggregationRelation->findData(oldAggregation.value(QStringLiteral("relationship_field_id")).toString());
    if (relationIndex >= 0) aggregationRelation->setCurrentIndex(relationIndex);
    int operationIndex = aggregationOperation->findData(oldAggregation.value(QStringLiteral("operation")).toString());
    if (operationIndex >= 0) aggregationOperation->setCurrentIndex(operationIndex);
    aggregationRelation->setVisible(formulaField);
    aggregationOperation->setVisible(formulaField);
    aggregationField->setVisible(formulaField);
    if (formulaField) {
        form->addRow(QStringLiteral("Aggregate related objects"), aggregationRelation);
        form->addRow(QStringLiteral("Aggregation"), aggregationOperation);
        form->addRow(QStringLiteral("Numeric target field"), aggregationField);
    }
    auto refreshAggregationFields = [this, aggregationRelation, aggregationField, oldAggregation] {
        const QString previousId = aggregationField->currentData().toString().isEmpty()
            ? oldAggregation.value(QStringLiteral("field_id")).toString() : aggregationField->currentData().toString();
        aggregationField->clear();
        QString allowedType;
        for (const ObjectType &type : std::as_const(m_types)) {
            for (const FieldDefinition &field : type.fields) {
                if (field.id == aggregationRelation->currentData().toString())
                    allowedType = field.options.value(QStringLiteral("allowed_type_id")).toString();
            }
        }
        for (const ObjectType &type : std::as_const(m_types)) {
            if (!allowedType.isEmpty() && type.id != allowedType) continue;
            for (const FieldDefinition &field : type.fields) {
                if (!field.archived && (field.fieldType == QStringLiteral("integer") || field.fieldType == QStringLiteral("decimal") ||
                    field.fieldType == QStringLiteral("percentage") || field.fieldType == QStringLiteral("progress") ||
                    field.fieldType == QStringLiteral("formula")))
                    aggregationField->addItem(type.name + QStringLiteral(" · ") + field.name, field.id);
            }
        }
        const int selected = aggregationField->findData(previousId);
        if (selected >= 0) aggregationField->setCurrentIndex(selected);
    };
    connect(aggregationRelation, &QComboBox::currentIndexChanged, &dialog, refreshAggregationFields);
    refreshAggregationFields();
    auto updateAggregationVisibility = [aggregationRelation, aggregationOperation, aggregationField] {
        const bool active = !aggregationRelation->currentData().toString().isEmpty();
        aggregationOperation->setVisible(active);
        aggregationField->setVisible(active && aggregationOperation->currentData().toString() != QStringLiteral("count"));
    };
    connect(aggregationRelation, &QComboBox::currentIndexChanged, &dialog, updateAggregationVisibility);
    connect(aggregationOperation, &QComboBox::currentIndexChanged, &dialog, updateAggregationVisibility);
    updateAggregationVisibility();

    auto *sampleObject = new QComboBox(&dialog);
    sampleObject->addItem(QStringLiteral("Choose a sample object…"), QString());
    for (const ObjectRecord &object : std::as_const(m_objects))
        if (object.typeId == typeId) sampleObject->addItem(object.name, object.id);
    auto *previewButton = button(QStringLiteral("Preview Formula"), QStringLiteral("softButton"), &dialog);
    auto *previewResult = textLabel(QStringLiteral("Preview uses the selected active object."), {}, &dialog);
    if (formulaField) {
        form->addRow(QStringLiteral("Sample object"), sampleObject);
        form->addRow(previewButton, previewResult);
    }
    previewButton->setVisible(formulaField);
    previewResult->setVisible(formulaField);
    connect(previewButton, &QPushButton::clicked, &dialog, [this, typeId, original, formulaExpression, aggregationRelation,
        aggregationOperation, aggregationField, sampleObject, previewResult] {
        const QString objectId = sampleObject->currentData().toString();
        if (objectId.isEmpty()) {
            previewResult->setText(QStringLiteral("Choose a sample object first."));
            return;
        }
        QJsonObject options{{QStringLiteral("expression"), formulaExpression->toPlainText()}};
        if (!aggregationRelation->currentData().toString().isEmpty()) {
            QJsonObject aggregation{{QStringLiteral("relationship_field_id"), aggregationRelation->currentData().toString()},
                                    {QStringLiteral("operation"), aggregationOperation->currentData().toString()}};
            if (aggregationOperation->currentData().toString() != QStringLiteral("count"))
                aggregation.insert(QStringLiteral("field_id"), aggregationField->currentData().toString());
            options.insert(QStringLiteral("aggregation"), aggregation);
        }
        QVector<ObjectType> previewTypes = m_types;
        auto previewType = std::find_if(previewTypes.begin(), previewTypes.end(), [&](const ObjectType &type) { return type.id == typeId; });
        if (previewType == previewTypes.end()) return;
        auto previewField = std::find_if(previewType->fields.begin(), previewType->fields.end(), [&](const FieldDefinition &field) { return field.id == original.id; });
        if (previewField == previewType->fields.end()) return;
        previewField->options = options;
        QVector<ObjectRecord> previewObjects = m_objects;
        auto object = std::find_if(previewObjects.begin(), previewObjects.end(), [&](const ObjectRecord &item) { return item.id == objectId; });
        if (object == previewObjects.end()) return;
        FormulaEvaluator::evaluate(previewType->fields, *object, previewTypes, previewObjects);
        if (object->formulaErrors.contains(original.id)) previewResult->setText(QStringLiteral("Preview error: %1").arg(object->formulaErrors.value(original.id)));
        else previewResult->setText(QStringLiteral("Result: %1").arg(object->values.value(original.id).toDouble(), 0, 'g', 12));
    });

    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *save = buttons->addButton(QStringLiteral("Save Field"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("goldButton"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [this, &dialog, original, name, icon, description, sortOrder, defaultValue,
        supportsDefault, choices, targetType, formulaExpression, aggregationRelation, aggregationOperation, aggregationField] {
        if (name->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Field name required"), QStringLiteral("Enter a name for this field."));
            name->setFocus();
            return;
        }
        FieldDefinition edited = original;
        edited.name = name->text();
        edited.icon = icon->text();
        edited.description = description->text();
        edited.sortOrder = sortOrder->value();
        QMap<QString, QString> replacements;
        if (edited.fieldType == QStringLiteral("select")) {
            QJsonArray choiceValues;
            QStringList values;
            QSet<QString> unique;
            for (const QString &line : choices->toPlainText().split(QLatin1Char('\n'))) {
                const QString choice = line.trimmed();
                if (choice.isEmpty()) continue;
                if (choice.size() > 100 || unique.contains(choice) || choiceValues.size() >= 200) {
                    QMessageBox::warning(&dialog, QStringLiteral("Invalid choices"), QStringLiteral("Choices must be unique, nonempty, at most 100 characters, with no more than 200 values."));
                    return;
                }
                unique.insert(choice);
                values.append(choice);
                choiceValues.append(choice);
            }
            if (values.isEmpty()) {
                QMessageBox::warning(&dialog, QStringLiteral("Choices required"), QStringLiteral("Keep at least one select choice."));
                return;
            }
            edited.options.insert(QStringLiteral("choices"), choiceValues);
            for (const QJsonValue &oldChoice : original.options.value(QStringLiteral("choices")).toArray()) {
                const QString removed = oldChoice.toString();
                if (unique.contains(removed)) continue;
                bool okay = false;
                const QString replacement = QInputDialog::getItem(&dialog, QStringLiteral("Replace select value"),
                    QStringLiteral("Choose a replacement for the existing value “%1”. This includes archived objects and the field default.").arg(removed),
                    values, 0, false, &okay);
                if (!okay) return;
                replacements.insert(removed, replacement);
            }
        }
        if (edited.fieldType == QStringLiteral("reference") || edited.fieldType == QStringLiteral("references")) {
            const QString allowed = targetType->currentData().toString();
            if (allowed.isEmpty()) edited.options.remove(QStringLiteral("allowed_type_id"));
            else edited.options.insert(QStringLiteral("allowed_type_id"), allowed);
        }
        if (edited.fieldType == QStringLiteral("formula")) {
            QJsonObject options{{QStringLiteral("expression"), formulaExpression->toPlainText()}};
            if (!aggregationRelation->currentData().toString().isEmpty()) {
                QJsonObject aggregation{{QStringLiteral("relationship_field_id"), aggregationRelation->currentData().toString()},
                                        {QStringLiteral("operation"), aggregationOperation->currentData().toString()}};
                if (aggregationOperation->currentData().toString() != QStringLiteral("count"))
                    aggregation.insert(QStringLiteral("field_id"), aggregationField->currentData().toString());
                options.insert(QStringLiteral("aggregation"), aggregation);
            }
            edited.options = options;
        }
        if (supportsDefault) {
            const QString raw = defaultValue->text().trimmed();
            if (raw.isEmpty()) edited.defaultValue = QJsonValue();
            else if (edited.fieldType == QStringLiteral("text") || edited.fieldType == QStringLiteral("long_text") ||
                     edited.fieldType == QStringLiteral("select") || edited.fieldType == QStringLiteral("date")) edited.defaultValue = raw;
            else if (edited.fieldType == QStringLiteral("boolean")) {
                if (raw.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) edited.defaultValue = true;
                else if (raw.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) edited.defaultValue = false;
                else {
                    QMessageBox::warning(&dialog, QStringLiteral("Invalid default"), QStringLiteral("Enter true or false for a checkbox default."));
                    return;
                }
            } else if (edited.fieldType == QStringLiteral("progress")) {
                QJsonParseError parseError;
                const QJsonDocument parsed = QJsonDocument::fromJson(raw.toUtf8(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
                    QMessageBox::warning(&dialog, QStringLiteral("Invalid default"), QStringLiteral("Enter progress as JSON, for example {\"current\":0,\"max\":10}."));
                    return;
                }
                edited.defaultValue = parsed.object();
            } else {
                bool okay = false;
                const double number = raw.toDouble(&okay);
                if (!okay || !std::isfinite(number)) {
                    QMessageBox::warning(&dialog, QStringLiteral("Invalid default"), QStringLiteral("Enter a finite number."));
                    return;
                }
                edited.defaultValue = number;
            }
        }
        QString error;
        if (!m_repository.updateField(edited, replacements, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not update field"), error);
            return;
        }
        dialog.accept();
    });
    name->setFocus();
    if (dialog.exec() == QDialog::Accepted) refreshGames(m_selectedGameId);
}

void MainWindow::openFieldTypeMigration(const QString &typeId, const QString &fieldId)
{
    const auto typeIt = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &type) { return type.id == typeId; });
    if (typeIt == m_types.cend() || typeIt->archived) return;
    const auto fieldIt = std::find_if(typeIt->fields.cbegin(), typeIt->fields.cend(), [&](const FieldDefinition &field) { return field.id == fieldId; });
    if (fieldIt == typeIt->fields.cend() || fieldIt->archived) return;
    const ObjectType type = *typeIt;
    const FieldDefinition field = *fieldIt;

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("fieldTypeMigrationDialog"));
    dialog.setWindowTitle(QStringLiteral("Change Field Type · %1").arg(field.name));
    dialog.resize(760, 700);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = textLabel(QStringLiteral("Preview active and archived object values, defaults, and dependencies before applying this atomic migration."));
    layout->addWidget(intro);
    auto *form = new QFormLayout();
    auto *target = new QComboBox(&dialog);
    target->setObjectName(QStringLiteral("migrationTargetType"));
    const QStringList fieldTypes{QStringLiteral("text"), QStringLiteral("long_text"), QStringLiteral("integer"),
        QStringLiteral("decimal"), QStringLiteral("percentage"), QStringLiteral("boolean"), QStringLiteral("select"),
        QStringLiteral("date"), QStringLiteral("progress"), QStringLiteral("reference"), QStringLiteral("references"), QStringLiteral("formula")};
    for (const QString &item : fieldTypes) target->addItem(item, item);
    const int firstTarget = target->findData(field.fieldType == QStringLiteral("formula") ? QStringLiteral("decimal") : QStringLiteral("text"));
    target->setCurrentIndex(firstTarget == target->findData(field.fieldType) ? (firstTarget + 1) % target->count() : firstTarget);
    form->addRow(QStringLiteral("Current field"), textLabel(QStringLiteral("%1 · %2").arg(field.name, field.fieldType)));
    form->addRow(QStringLiteral("New type"), target);

    auto *choices = new QTextEdit(&dialog);
    choices->setObjectName(QStringLiteral("migrationTargetChoices"));
    choices->setMaximumHeight(90);
    QStringList priorChoices;
    for (const QJsonValue &choice : field.options.value(QStringLiteral("choices")).toArray()) priorChoices.append(choice.toString());
    choices->setPlainText(priorChoices.join(QLatin1Char('\n')));
    form->addRow(QStringLiteral("Target choices (one per line)"), choices);

    auto *allowedType = new QComboBox(&dialog);
    allowedType->setObjectName(QStringLiteral("migrationAllowedType"));
    allowedType->addItem(QStringLiteral("Any type in this game"), QString());
    for (const ObjectType &candidate : std::as_const(m_types))
        if (!candidate.archived) allowedType->addItem(candidate.name, candidate.id);
    const int currentAllowed = allowedType->findData(field.options.value(QStringLiteral("allowed_type_id")).toString());
    if (currentAllowed >= 0) allowedType->setCurrentIndex(currentAllowed);
    form->addRow(QStringLiteral("Allowed reference type"), allowedType);

    auto *maximum = new QDoubleSpinBox(&dialog);
    maximum->setObjectName(QStringLiteral("migrationProgressMaximum"));
    maximum->setDecimals(6);
    maximum->setRange(0.000001, 1000000000000.0);
    maximum->setValue(100.0);
    form->addRow(QStringLiteral("Progress maximum"), maximum);
    auto *progressValue = new QComboBox(&dialog);
    progressValue->setObjectName(QStringLiteral("migrationProgressValue"));
    progressValue->addItem(QStringLiteral("Choose a value strategy…"), QString());
    progressValue->addItem(QStringLiteral("Current value"), QStringLiteral("current"));
    progressValue->addItem(QStringLiteral("Maximum value"), QStringLiteral("maximum"));
    progressValue->addItem(QStringLiteral("Current ÷ maximum × 100"), QStringLiteral("percentage"));
    form->addRow(QStringLiteral("Progress conversion"), progressValue);
    auto *serializeLinks = new QCheckBox(QStringLiteral("Store linked object IDs as JSON text"), &dialog);
    serializeLinks->setObjectName(QStringLiteral("migrationSerializeLinks"));
    form->addRow(QStringLiteral("Reference conversion"), serializeLinks);
    auto *clamp = new QCheckBox(QStringLiteral("Clamp values outside 0–100 to the nearest boundary"), &dialog);
    clamp->setObjectName(QStringLiteral("migrationClampPercentage"));
    form->addRow(QStringLiteral("Percentage conversion"), clamp);
    auto *freeze = new QCheckBox(QStringLiteral("Freeze each current formula result into a stored value"), &dialog);
    freeze->setObjectName(QStringLiteral("migrationFreezeFormula"));
    form->addRow(QStringLiteral("Formula conversion"), freeze);

    auto *defaultAction = new QComboBox(&dialog);
    defaultAction->setObjectName(QStringLiteral("migrationDefaultAction"));
    defaultAction->addItem(QStringLiteral("Convert the current default automatically"), QStringLiteral("convert"));
    defaultAction->addItem(QStringLiteral("Clear the default"), QStringLiteral("clear"));
    defaultAction->addItem(QStringLiteral("Set a new target default"), QStringLiteral("set"));
    form->addRow(QStringLiteral("Default value"), defaultAction);
    auto *defaultInput = new QLineEdit(&dialog);
    defaultInput->setObjectName(QStringLiteral("migrationDefaultValue"));
    defaultInput->setPlaceholderText(QStringLiteral("Text, number, true/false, or progress JSON"));
    form->addRow(QStringLiteral("New default"), defaultInput);
    layout->addLayout(form);

    auto setControlVisibility = [=] {
        const QString to = target->currentData().toString();
        const bool numeric = to == QStringLiteral("integer") || to == QStringLiteral("decimal") || to == QStringLiteral("percentage");
        const bool fromNumeric = field.fieldType == QStringLiteral("integer") || field.fieldType == QStringLiteral("decimal") || field.fieldType == QStringLiteral("percentage");
        choices->setVisible(to == QStringLiteral("select"));
        form->labelForField(choices)->setVisible(to == QStringLiteral("select"));
        allowedType->setVisible(to == QStringLiteral("reference") || to == QStringLiteral("references"));
        form->labelForField(allowedType)->setVisible(allowedType->isVisible());
        maximum->setVisible(to == QStringLiteral("progress") && fromNumeric);
        form->labelForField(maximum)->setVisible(maximum->isVisible());
        progressValue->setVisible(field.fieldType == QStringLiteral("progress") && numeric);
        form->labelForField(progressValue)->setVisible(progressValue->isVisible());
        serializeLinks->setVisible((field.fieldType == QStringLiteral("reference") || field.fieldType == QStringLiteral("references")) &&
            (to == QStringLiteral("text") || to == QStringLiteral("long_text")));
        form->labelForField(serializeLinks)->setVisible(serializeLinks->isVisible());
        clamp->setVisible(to == QStringLiteral("percentage"));
        form->labelForField(clamp)->setVisible(clamp->isVisible());
        freeze->setVisible(field.fieldType == QStringLiteral("formula") && numeric);
        form->labelForField(freeze)->setVisible(freeze->isVisible());
        const bool supportsDefault = to != QStringLiteral("formula") && to != QStringLiteral("reference") && to != QStringLiteral("references");
        defaultAction->setVisible(supportsDefault);
        form->labelForField(defaultAction)->setVisible(supportsDefault);
        defaultInput->setVisible(supportsDefault && defaultAction->currentData().toString() == QStringLiteral("set"));
        form->labelForField(defaultInput)->setVisible(defaultInput->isVisible());
    };
    connect(target, &QComboBox::currentIndexChanged, &dialog, setControlVisibility);
    connect(defaultAction, &QComboBox::currentIndexChanged, &dialog, setControlVisibility);
    setControlVisibility();

    auto *mappings = new QWidget(&dialog);
    mappings->setObjectName(QStringLiteral("migrationValueMappings"));
    auto *mappingLayout = new QVBoxLayout(mappings);
    mappingLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(mappings);
    auto *previewText = new QTextEdit(&dialog);
    previewText->setObjectName(QStringLiteral("fieldMigrationPreviewText"));
    previewText->setReadOnly(true);
    previewText->setMinimumHeight(150);
    previewText->setPlainText(QStringLiteral("Choose a target type and preview the conversion."));
    layout->addWidget(previewText, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *previewButton = buttons->addButton(QStringLiteral("Preview Conversion"), QDialogButtonBox::ActionRole);
    previewButton->setObjectName(QStringLiteral("previewFieldMigrationButton"));
    auto *applyButton = buttons->addButton(QStringLiteral("Apply Migration"), QDialogButtonBox::AcceptRole);
    applyButton->setObjectName(QStringLiteral("applyFieldMigrationButton"));
    applyButton->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QString previewToken;
    auto requestForCurrentControls = [&]() {
        FieldMigrationRequest request;
        request.targetType = target->currentData().toString();
        if (request.targetType == QStringLiteral("select")) {
            QJsonArray targetChoices;
            for (const QString &line : choices->toPlainText().split(QLatin1Char('\n'))) {
                const QString choice = line.trimmed();
                if (!choice.isEmpty()) targetChoices.append(choice);
            }
            request.targetOptions.insert(QStringLiteral("choices"), targetChoices);
        } else if (request.targetType == QStringLiteral("reference") || request.targetType == QStringLiteral("references")) {
            const QString allowed = allowedType->currentData().toString();
            if (!allowed.isEmpty()) request.targetOptions.insert(QStringLiteral("allowed_type_id"), allowed);
        }
        if (maximum->isVisible()) request.policy.insert(QStringLiteral("maximum"), maximum->value());
        if (progressValue->isVisible() && !progressValue->currentData().toString().isEmpty())
            request.policy.insert(QStringLiteral("progress_value"), progressValue->currentData().toString());
        if (serializeLinks->isVisible() && serializeLinks->isChecked()) request.policy.insert(QStringLiteral("relationship_strategy"), QStringLiteral("serialize_ids"));
        if (clamp->isVisible() && clamp->isChecked()) request.policy.insert(QStringLiteral("percentage_strategy"), QStringLiteral("clamp"));
        if (freeze->isVisible() && freeze->isChecked()) request.policy.insert(QStringLiteral("freeze_formula"), true);
        QJsonObject replacements;
        QJsonObject referenceMappings;
        for (QComboBox *combo : mappings->findChildren<QComboBox *>()) {
            const QString selected = combo->currentData().toString();
            const QString source = combo->property("migrationSourceValue").toString();
            if (selected.isEmpty() || source.isNull()) continue;
            if (combo->objectName() == QStringLiteral("selectReplacementChoice")) replacements.insert(source, selected);
            else if (combo->objectName() == QStringLiteral("referenceReplacementObject")) referenceMappings.insert(source, selected);
        }
        if (!replacements.isEmpty()) request.policy.insert(QStringLiteral("select_replacements"), replacements);
        if (!referenceMappings.isEmpty()) request.policy.insert(QStringLiteral("reference_mapping"), referenceMappings);
        const QString action = defaultAction->currentData().toString();
        if (defaultAction->isVisible() && action != QStringLiteral("convert")) {
            request.defaultOverrideProvided = true;
            if (action == QStringLiteral("clear")) request.defaultOverride = QJsonValue(QJsonValue::Null);
            else {
                const QString text = defaultInput->text().trimmed();
                const QString to = request.targetType;
                if (to == QStringLiteral("text") || to == QStringLiteral("long_text") || to == QStringLiteral("select") || to == QStringLiteral("date")) request.defaultOverride = text;
                else if (to == QStringLiteral("boolean")) request.defaultOverride = text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ? QJsonValue(true)
                    : text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0 ? QJsonValue(false) : QJsonValue();
                else if (to == QStringLiteral("progress")) {
                    QJsonParseError parseError;
                    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
                    request.defaultOverride = parseError.error == QJsonParseError::NoError && document.isObject() ? QJsonValue(document.object()) : QJsonValue();
                } else {
                    bool validNumber = false;
                    const double number = text.toDouble(&validNumber);
                    request.defaultOverride = validNumber && std::isfinite(number) ? QJsonValue(number) : QJsonValue();
                }
            }
        }
        return request;
    };
    auto invalidatePreview = [&] {
        previewToken.clear();
        applyButton->setEnabled(false);
        previewText->setPlainText(QStringLiteral("Settings changed. Preview the conversion again before applying."));
    };
    connect(target, &QComboBox::currentIndexChanged, &dialog, [invalidatePreview] { invalidatePreview(); });
    connect(choices, &QTextEdit::textChanged, &dialog, [invalidatePreview] { invalidatePreview(); });
    connect(allowedType, &QComboBox::currentIndexChanged, &dialog, [invalidatePreview] { invalidatePreview(); });
    connect(maximum, &QDoubleSpinBox::valueChanged, &dialog, [invalidatePreview] { invalidatePreview(); });
    connect(progressValue, &QComboBox::currentIndexChanged, &dialog, [invalidatePreview] { invalidatePreview(); });
    connect(serializeLinks, &QCheckBox::toggled, &dialog, [invalidatePreview] { invalidatePreview(); });
    connect(clamp, &QCheckBox::toggled, &dialog, [invalidatePreview] { invalidatePreview(); });
    connect(freeze, &QCheckBox::toggled, &dialog, [invalidatePreview] { invalidatePreview(); });
    connect(defaultAction, &QComboBox::currentIndexChanged, &dialog, [invalidatePreview] { invalidatePreview(); });
    connect(defaultInput, &QLineEdit::textChanged, &dialog, [invalidatePreview] { invalidatePreview(); });

    connect(previewButton, &QPushButton::clicked, &dialog, [&, requestForCurrentControls] {
        const FieldMigrationRequest request = requestForCurrentControls();
        QString error;
        const FieldMigrationPreview result = m_repository.previewFieldTypeMigration(fieldId, request, &error);
        while (QLayoutItem *item = mappingLayout->takeAt(0)) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        if (!error.isEmpty()) {
            previewToken.clear();
            applyButton->setEnabled(false);
            previewText->setPlainText(error);
            return;
        }
        QStringList lines;
        lines << QStringLiteral("%1 → %2").arg(result.previousType, result.targetType)
              << QStringLiteral("Affected objects: %1 · Saved values: %2 · Compatible: %3 · Incompatible: %4 · Archived objects: %5")
                    .arg(result.affectedObjects).arg(result.savedValues).arg(result.compatibleValues).arg(result.incompatibleValues).arg(result.archivedObjectsAffected)
              << QStringLiteral("Preset overrides: %1").arg(result.presetOverrides)
              << QStringLiteral("Default: %1 → %2").arg(field.defaultValue.isUndefined() || field.defaultValue.isNull() ? QStringLiteral("none")
                    : QString::fromUtf8(QJsonDocument(QJsonArray{field.defaultValue}).toJson(QJsonDocument::Compact).mid(1).chopped(1)),
                    result.convertedDefault.isUndefined() || result.convertedDefault.isNull() ? QStringLiteral("none")
                    : QString::fromUtf8(QJsonDocument(QJsonArray{result.convertedDefault}).toJson(QJsonDocument::Compact).mid(1).chopped(1)));
        if (!result.formulaWarning.isEmpty()) lines << QStringLiteral("Warning: %1").arg(result.formulaWarning);
        if (result.dependencies.isEmpty()) lines << QStringLiteral("Dependencies: none");
        else lines << QStringLiteral("Dependencies: %1").arg(result.dependencies.join(QStringLiteral(", ")));
        for (const QString &blocker : result.blockers) lines << QStringLiteral("BLOCKED: %1").arg(blocker);
        for (const FieldMigrationExample &example : result.examples) {
            const QString raw = example.value.isUndefined() || example.value.isNull() ? QStringLiteral("null")
                : QString::fromUtf8(QJsonDocument(QJsonArray{example.value}).toJson(QJsonDocument::Compact).mid(1).chopped(1));
            lines << QStringLiteral("%1 (%2): %3 — %4").arg(example.objectName, example.objectId, raw, example.reason);
            if (request.targetType == QStringLiteral("select") && (field.fieldType == QStringLiteral("text") || field.fieldType == QStringLiteral("long_text")) && example.value.isString()) {
                auto *replacement = new QComboBox(mappings);
                replacement->setObjectName(QStringLiteral("selectReplacementChoice"));
                replacement->setProperty("migrationSourceValue", example.value.toString());
                replacement->addItem(QStringLiteral("Replace “%1” with…").arg(example.value.toString()), QString());
                for (const QJsonValue &choice : request.targetOptions.value(QStringLiteral("choices")).toArray()) replacement->addItem(choice.toString(), choice.toString());
                mappingLayout->addWidget(replacement);
                connect(replacement, &QComboBox::currentIndexChanged, &dialog, [invalidatePreview] { invalidatePreview(); });
            } else if ((request.targetType == QStringLiteral("reference") || request.targetType == QStringLiteral("references")) &&
                       (field.fieldType == QStringLiteral("text") || field.fieldType == QStringLiteral("long_text") || field.fieldType == QStringLiteral("select")) && example.value.isString()) {
                auto *replacement = new QComboBox(mappings);
                replacement->setObjectName(QStringLiteral("referenceReplacementObject"));
                replacement->setProperty("migrationSourceValue", example.value.toString());
                replacement->addItem(QStringLiteral("Map “%1” to an object…").arg(example.value.toString()), QString());
                const QString allowed = request.targetOptions.value(QStringLiteral("allowed_type_id")).toString();
                for (const ObjectRecord &candidate : std::as_const(m_objects)) {
                    if (!allowed.isEmpty() && candidate.typeId != allowed) continue;
                    const auto candidateType = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &item) { return item.id == candidate.typeId; });
                    replacement->addItem(QStringLiteral("%1 · %2").arg(candidate.name, candidateType == m_types.cend() ? candidate.typeId : candidateType->name), candidate.id);
                }
                mappingLayout->addWidget(replacement);
                connect(replacement, &QComboBox::currentIndexChanged, &dialog, [invalidatePreview] { invalidatePreview(); });
            }
        }
        previewText->setPlainText(lines.join(QLatin1Char('\n')));
        previewToken = result.previewToken;
        applyButton->setEnabled(result.canApply);
        if (!result.canApply && result.incompatibleValues > 0)
            previewText->append(QStringLiteral("Edit incompatible object values in their normal editor, or choose an explicit replacement above, then preview again."));
    });
    connect(applyButton, &QPushButton::clicked, &dialog, [&, requestForCurrentControls] {
        if (previewToken.isEmpty()) return;
        const FieldMigrationRequest request = requestForCurrentControls();
        QString error;
        FieldMigrationPreview result;
        if (!m_repository.applyFieldTypeMigration(fieldId, request, previewToken, &result, &error)) {
            applyButton->setEnabled(false);
            previewToken.clear();
            previewText->append(QStringLiteral("\nMigration was not applied: %1").arg(error));
            return;
        }
        QMessageBox::information(&dialog, QStringLiteral("Field migration complete"),
            QStringLiteral("%1 values converted across %2 objects. The migration and Object History events were saved atomically.").arg(result.savedValues).arg(result.affectedObjects));
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted) refreshGames(m_selectedGameId);
}

void MainWindow::openObjectEditor(const QString &objectId)
{
    if (m_selectedGameId.isEmpty()) return;
    if (m_types.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Create an object type first"), QStringLiteral("Object types define the fields available on each object."));
        selectPage(Page::Types);
        openCreateType();
        return;
    }

    ObjectRecord original;
    const bool editing = !objectId.isEmpty();
    if (editing) {
        const auto found = std::find_if(m_objects.cbegin(), m_objects.cend(), [&](const ObjectRecord &object) { return object.id == objectId; });
        if (found == m_objects.cend()) return;
        original = *found;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(editing ? QStringLiteral("Edit Object") : QStringLiteral("Create Object"));
    dialog.setMinimumSize(560, 500);
    auto *outer = new QVBoxLayout(&dialog);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget(scroll);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(14, 13, 14, 13);
    auto *metadata = new QFormLayout();
    auto *name = new QLineEdit(editing ? original.name : QString(), body);
    name->setMaxLength(160);
    auto *description = new QLineEdit(editing ? original.description : QString(), body);
    description->setMaxLength(500);
    auto *image = new QLineEdit(editing ? original.image : QString(), body);
    image->setObjectName(QStringLiteral("objectImageInput"));
    image->setReadOnly(true);
    auto *imageRow = new QWidget(body);
    auto *imageLayout = new QHBoxLayout(imageRow);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    imageLayout->addWidget(image, 1);
    auto *chooseImage = button(QStringLiteral("Choose…"), QStringLiteral("softButton"), imageRow);
    imageLayout->addWidget(chooseImage);
    auto *clearImage = button(QStringLiteral("Clear"), QStringLiteral("softButton"), imageRow);
    imageLayout->addWidget(clearImage);
    connect(clearImage, &QPushButton::clicked, &dialog, [image] { image->clear(); });
    connect(chooseImage, &QPushButton::clicked, &dialog, [this, &dialog, image] {
        const QString reference = openAssetLibrary(&dialog, image->text());
        if (!reference.isNull()) image->setText(reference);
    });
    metadata->addRow(QStringLiteral("Object name"), name);
    metadata->addRow(QStringLiteral("Description"), description);
    metadata->addRow(QStringLiteral("Image"), imageRow);
    auto *typeChoice = new QComboBox(body);
    for (const ObjectType &type : std::as_const(m_types)) typeChoice->addItem(type.name, type.id);
    if (editing) {
        const int index = typeChoice->findData(original.typeId);
        if (index >= 0) typeChoice->setCurrentIndex(index);
        typeChoice->setEnabled(false);
    }
    metadata->addRow(QStringLiteral("Object type"), typeChoice);
    bodyLayout->addLayout(metadata);
    auto *fieldContainer = new QWidget(body);
    auto *fieldForm = new QFormLayout(fieldContainer);
    fieldForm->setContentsMargins(0, 4, 0, 4);
    fieldForm->setSpacing(8);
    bodyLayout->addWidget(fieldContainer);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    struct FieldInput {
        FieldDefinition field;
        QPointer<QWidget> control;
        QPointer<QWidget> secondary;
    };
    QMap<QString, FieldInput> controls;
    auto rebuildFields = [this, editing, &original, typeChoice, fieldForm, fieldContainer, &controls] {
        while (QLayoutItem *item = fieldForm->takeAt(0)) {
            if (QWidget *widget = item->widget()) delete widget;
            delete item;
        }
        controls.clear();
        const QString typeId = typeChoice->currentData().toString();
        const auto typeIt = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &type) { return type.id == typeId; });
        if (typeIt == m_types.cend()) return;
        const QVector<ObjectRecord> allObjects = m_repository.objects(m_selectedGameId);
        for (const FieldDefinition &field : typeIt->fields) {
            if (field.archived) continue;
            QWidget *control = nullptr;
            QWidget *secondary = nullptr;
            const QJsonValue value = editing ? original.values.value(field.id) : field.defaultValue;
            const QStringList refs = editing ? original.references.value(field.id) : QStringList();
            const QString labelText = QStringLiteral("%1  %2").arg(field.icon, field.name);
            if (field.fieldType == QStringLiteral("text")) {
                auto *input = new QLineEdit(fieldContainer);
                if (value.isString()) input->setText(value.toString());
                input->setMaxLength(2000);
                control = input;
            } else if (field.fieldType == QStringLiteral("long_text")) {
                auto *input = new QTextEdit(fieldContainer);
                if (value.isString()) input->setPlainText(value.toString());
                input->setMaximumHeight(90);
                control = input;
            } else if (field.fieldType == QStringLiteral("integer") || field.fieldType == QStringLiteral("decimal") || field.fieldType == QStringLiteral("percentage")) {
                auto *input = new QLineEdit(fieldContainer);
                if (field.fieldType == QStringLiteral("integer")) input->setValidator(new QIntValidator(input));
                else if (field.fieldType == QStringLiteral("percentage")) input->setValidator(new QDoubleValidator(0, 100, 8, input));
                else input->setValidator(new QDoubleValidator(input));
                if (value.isDouble()) input->setText(value.toVariant().toString());
                input->setPlaceholderText(QStringLiteral("Optional"));
                control = input;
            } else if (field.fieldType == QStringLiteral("boolean")) {
                auto *input = new QCheckBox(QStringLiteral("Enabled"), fieldContainer);
                input->setChecked(value.isBool() && value.toBool());
                control = input;
            } else if (field.fieldType == QStringLiteral("select")) {
                auto *input = new QComboBox(fieldContainer);
                input->addItem(QStringLiteral("Choose…"), QVariant());
                for (const QJsonValue &choice : field.options.value(QStringLiteral("choices")).toArray()) input->addItem(choice.toString(), choice.toString());
                if (value.isString()) input->setCurrentIndex(input->findData(value.toString()));
                control = input;
            } else if (field.fieldType == QStringLiteral("date")) {
                auto *container = new QWidget(fieldContainer);
                auto *row = new QHBoxLayout(container);
                row->setContentsMargins(0, 0, 0, 0);
                auto *enabled = new QCheckBox(QStringLiteral("Set date"), container);
                auto *date = new QDateEdit(QDate::currentDate(), container);
                date->setCalendarPopup(true);
                date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
                date->setEnabled(false);
                if (value.isString()) {
                    const QDate parsed = QDate::fromString(value.toString(), Qt::ISODate);
                    if (parsed.isValid()) { date->setDate(parsed); date->setEnabled(true); enabled->setChecked(true); }
                }
                connect(enabled, &QCheckBox::toggled, date, &QWidget::setEnabled);
                row->addWidget(enabled);
                row->addWidget(date);
                row->addStretch(1);
                control = enabled;
                secondary = date;
                fieldForm->addRow(labelText, container);
                controls.insert(field.id, FieldInput{field, control, secondary});
                continue;
            } else if (field.fieldType == QStringLiteral("progress")) {
                auto *container = new QWidget(fieldContainer);
                auto *row = new QHBoxLayout(container);
                row->setContentsMargins(0, 0, 0, 0);
                auto *current = new QLineEdit(container);
                auto *maximum = new QLineEdit(container);
                current->setValidator(new QDoubleValidator(current));
                maximum->setValidator(new QDoubleValidator(maximum));
                current->setPlaceholderText(QStringLiteral("Current"));
                maximum->setPlaceholderText(QStringLiteral("Maximum"));
                if (value.isObject()) {
                    current->setText(value.toObject().value(QStringLiteral("current")).toVariant().toString());
                    maximum->setText(value.toObject().value(QStringLiteral("max")).toVariant().toString());
                }
                row->addWidget(current);
                row->addWidget(maximum);
                fieldForm->addRow(labelText, container);
                controls.insert(field.id, FieldInput{field, current, maximum});
                continue;
            } else if (field.fieldType == QStringLiteral("reference")) {
                auto *input = new QComboBox(fieldContainer);
                input->addItem(QStringLiteral("No reference"), QString());
                const QString allowedType = field.options.value(QStringLiteral("allowed_type_id")).toString();
                for (const ObjectRecord &target : allObjects) {
                    if (target.id == original.id || (!allowedType.isEmpty() && target.typeId != allowedType)) continue;
                    input->addItem(QStringLiteral("%1 · %2").arg(target.name, target.typeName), target.id);
                }
                for (const QString &referenceId : refs) {
                    if (input->findData(referenceId) < 0) input->addItem(QStringLiteral("Unavailable · %1").arg(referenceId), referenceId);
                }
                if (!refs.isEmpty()) input->setCurrentIndex(input->findData(refs.constFirst()));
                control = input;
            } else if (field.fieldType == QStringLiteral("references")) {
                auto *input = new QListWidget(fieldContainer);
                input->setMaximumHeight(110);
                const QString allowedType = field.options.value(QStringLiteral("allowed_type_id")).toString();
                for (const ObjectRecord &target : allObjects) {
                    if (target.id == original.id || (!allowedType.isEmpty() && target.typeId != allowedType)) continue;
                    auto *item = new QListWidgetItem(QStringLiteral("%1 · %2").arg(target.name, target.typeName), input);
                    item->setData(Qt::UserRole, target.id);
                    item->setSelected(refs.contains(target.id));
                }
                for (const QString &referenceId : refs) {
                    bool found = false;
                    for (int index = 0; index < input->count(); ++index) if (input->item(index)->data(Qt::UserRole).toString() == referenceId) found = true;
                    if (!found) {
                        auto *item = new QListWidgetItem(QStringLiteral("Unavailable · %1").arg(referenceId), input);
                        item->setData(Qt::UserRole, referenceId);
                        item->setSelected(true);
                    }
                }
                input->setSelectionMode(QAbstractItemView::MultiSelection);
                control = input;
            } else {
                QString formulaStatus = QStringLiteral("Calculated from this object's saved values.");
                if (editing && original.formulaErrors.contains(field.id))
                    formulaStatus = QStringLiteral("Formula error: %1").arg(original.formulaErrors.value(field.id));
                else if (editing && original.values.contains(field.id))
                    formulaStatus = QStringLiteral("Calculated value: %1").arg(original.values.value(field.id).toDouble(), 0, 'g', 12);
                auto *calculated = textLabel(formulaStatus);
                calculated->setStyleSheet(QStringLiteral("color:#715c74;font-size:11px;"));
                calculated->setEnabled(false);
                control = calculated;
            }
            fieldForm->addRow(labelText, control);
            controls.insert(field.id, FieldInput{field, control, secondary});
        }
        fieldContainer->updateGeometry();
    };

    connect(typeChoice, &QComboBox::currentIndexChanged, &dialog, [rebuildFields] { rebuildFields(); });
    rebuildFields();
    if (editing) typeChoice->setEnabled(false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *save = buttons->addButton(editing ? QStringLiteral("Save Object") : QStringLiteral("Create Object"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("goldButton"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [this, &dialog, editing, &original, name, description, image, typeChoice, &controls] {
        if (name->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Object name required"), QStringLiteral("Enter a name for this object."));
            name->setFocus();
            return;
        }
        ObjectRecord object = original;
        object.gameId = m_selectedGameId;
        object.typeId = typeChoice->currentData().toString();
        object.name = name->text();
        object.description = description->text();
        object.image = image->text();
        const auto typeIt = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &type) { return type.id == object.typeId; });
        if (typeIt == m_types.cend()) {
            QMessageBox::warning(&dialog, QStringLiteral("Object type missing"), QStringLiteral("Select an active object type."));
            return;
        }
        object.typeName = typeIt->name;
        for (auto it = controls.cbegin(); it != controls.cend(); ++it) {
            const FieldInput &input = it.value();
            const FieldDefinition &field = input.field;
            if (field.fieldType == QStringLiteral("formula")) continue;
            if (field.fieldType == QStringLiteral("reference")) {
                auto *combo = qobject_cast<QComboBox *>(input.control.data());
                const QString id = combo ? combo->currentData().toString() : QString();
                object.references[field.id] = id.isEmpty() ? QStringList() : QStringList{id};
                object.values.remove(field.id);
            } else if (field.fieldType == QStringLiteral("references")) {
                auto *list = qobject_cast<QListWidget *>(input.control.data());
                QStringList ids;
                if (list) for (QListWidgetItem *item : list->selectedItems()) ids.append(item->data(Qt::UserRole).toString());
                object.references[field.id] = ids;
                object.values.remove(field.id);
            } else if (field.fieldType == QStringLiteral("boolean")) {
                const auto *check = qobject_cast<QCheckBox *>(input.control.data());
                object.values[field.id] = check && check->isChecked();
            } else if (field.fieldType == QStringLiteral("long_text")) {
                const auto *text = qobject_cast<QTextEdit *>(input.control.data());
                const QString value = text ? text->toPlainText() : QString();
                object.values[field.id] = value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
            } else if (field.fieldType == QStringLiteral("text")) {
                auto *text = qobject_cast<QLineEdit *>(input.control.data());
                const QString value = text ? text->text() : QString();
                object.values[field.id] = value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
            } else if (field.fieldType == QStringLiteral("integer") || field.fieldType == QStringLiteral("decimal") || field.fieldType == QStringLiteral("percentage")) {
                auto *text = qobject_cast<QLineEdit *>(input.control.data());
                const QString raw = text ? text->text().trimmed() : QString();
                if (raw.isEmpty()) object.values[field.id] = QJsonValue(QJsonValue::Null);
                else {
                    bool okay = false;
                    const double number = raw.toDouble(&okay);
                    if (!okay || (field.fieldType == QStringLiteral("integer") && std::floor(number) != number)) {
                        QMessageBox::warning(&dialog, QStringLiteral("Invalid field value"), QStringLiteral("Enter a valid number for %1.").arg(field.name));
                        text->setFocus();
                        return;
                    }
                    object.values[field.id] = number;
                }
            } else if (field.fieldType == QStringLiteral("select")) {
                const auto *combo = qobject_cast<QComboBox *>(input.control.data());
                const QVariant value = combo ? combo->currentData() : QVariant();
                object.values[field.id] = value.isValid() && !value.toString().isEmpty() ? QJsonValue(value.toString()) : QJsonValue(QJsonValue::Null);
            } else if (field.fieldType == QStringLiteral("date")) {
                const auto *enabled = qobject_cast<QCheckBox *>(input.control.data());
                const auto *date = qobject_cast<QDateEdit *>(input.secondary.data());
                object.values[field.id] = enabled && enabled->isChecked() && date ? QJsonValue(date->date().toString(Qt::ISODate)) : QJsonValue(QJsonValue::Null);
            } else if (field.fieldType == QStringLiteral("progress")) {
                const auto *current = qobject_cast<QLineEdit *>(input.control.data());
                auto *maximum = qobject_cast<QLineEdit *>(input.secondary.data());
                const QString rawCurrent = current ? current->text().trimmed() : QString();
                const QString rawMaximum = maximum ? maximum->text().trimmed() : QString();
                if (rawCurrent.isEmpty() && rawMaximum.isEmpty()) object.values[field.id] = QJsonValue(QJsonValue::Null);
                else {
                    bool currentOkay = true;
                    bool maxOkay = true;
                    const double currentValue = rawCurrent.toDouble(&currentOkay);
                    const double maxValue = rawMaximum.toDouble(&maxOkay);
                    if (rawCurrent.isEmpty() || rawMaximum.isEmpty() || !currentOkay || !maxOkay || maxValue <= 0) {
                        QMessageBox::warning(&dialog, QStringLiteral("Invalid progress"), QStringLiteral("Enter both a current value and a positive maximum for %1.").arg(field.name));
                        maximum->setFocus();
                        return;
                    }
                    object.values[field.id] = QJsonObject{{QStringLiteral("current"), currentValue}, {QStringLiteral("max"), maxValue}};
                }
            }
        }
        QString error;
        const QString sectionId = editing ? QString() : m_selectedSectionId;
        if (!m_repository.saveObject(object, typeIt->fields, sectionId, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not save object"), error);
            return;
        }
        dialog.accept();
        refreshGames(m_selectedGameId);
    });
    name->setFocus();
    dialog.exec();
}

void MainWindow::openGoalEditor(const QString &goalId)
{
    if (m_selectedGameId.isEmpty()) return;
    const bool editing = !goalId.isEmpty();
    Goal original;
    if (editing) {
        const auto found = std::find_if(m_goals.cbegin(), m_goals.cend(), [&](const Goal &goal) { return goal.id == goalId; });
        if (found == m_goals.cend()) return;
        original = *found;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(editing ? QStringLiteral("Edit Goal") : QStringLiteral("Add Goal"));
    dialog.setMinimumSize(600, 680);
    auto *outer = new QVBoxLayout(&dialog);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget(scroll);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(12, 10, 12, 10);

    auto *form = new QFormLayout();
    auto *title = new QLineEdit(editing ? original.title : QString(), body);
    title->setObjectName(QStringLiteral("goalTitleInput"));
    title->setMaxLength(160);
    auto *description = new QTextEdit(body);
    description->setObjectName(QStringLiteral("goalDescriptionInput"));
    description->setMaximumHeight(85);
    if (editing) description->setPlainText(original.description);
    auto *kind = new QComboBox(body);
    kind->setObjectName(QStringLiteral("goalTypeInput"));
    kind->addItem(QStringLiteral("Manual milestone"), QStringLiteral("manual"));
    kind->addItem(QStringLiteral("Checklist"), QStringLiteral("checklist"));
    kind->addItem(QStringLiteral("Numeric target"), QStringLiteral("numeric"));
    kind->addItem(QStringLiteral("Object field target"), QStringLiteral("field_target"));
    if (editing) {
        const int index = kind->findData(original.goalType);
        if (index >= 0) kind->setCurrentIndex(index);
        kind->setEnabled(false);
    }
    form->addRow(QStringLiteral("Goal title"), title);
    form->addRow(QStringLiteral("Description"), description);
    form->addRow(QStringLiteral("Goal type"), kind);
    bodyLayout->addLayout(form);

    auto *numericPanel = panel(QStringLiteral("goalEditorNumeric"), body);
    auto *numericForm = new QFormLayout(numericPanel);
    auto *target = new QDoubleSpinBox(numericPanel);
    target->setRange(0.000001, 1.0e12);
    target->setDecimals(8);
    target->setValue(editing && original.target.isDouble() ? original.target.toDouble() : 1.0);
    auto *current = new QDoubleSpinBox(numericPanel);
    current->setRange(0, 1.0e12);
    current->setDecimals(8);
    current->setValue(editing ? original.current : 0.0);
    numericForm->addRow(QStringLiteral("Target"), target);
    numericForm->addRow(QStringLiteral("Current progress"), current);
    QWidget *currentLabel = numericForm->labelForField(current);
    bodyLayout->addWidget(numericPanel);

    auto *fieldPanel = panel(QStringLiteral("goalEditorField"), body);
    auto *fieldForm = new QFormLayout(fieldPanel);
    auto *object = new QComboBox(fieldPanel);
    object->setObjectName(QStringLiteral("goalObjectInput"));
    for (const ObjectRecord &record : std::as_const(m_objects))
        object->addItem(QStringLiteral("%1 · %2").arg(record.name, record.typeName), record.id);
    if (editing) {
        const int index = object->findData(original.objectId);
        if (index >= 0) object->setCurrentIndex(index);
    }
    auto *field = new QComboBox(fieldPanel);
    field->setObjectName(QStringLiteral("goalFieldInput"));
    fieldForm->addRow(QStringLiteral("Object"), object);
    fieldForm->addRow(QStringLiteral("Numeric field"), field);
    fieldForm->addRow(QStringLiteral("Target value"), new QLabel(QStringLiteral("Uses the target above"), fieldPanel));
    auto rebuildGoalFields = [this, object, field, editing, original] {
        const QString selectedObject = object->currentData().toString();
        field->clear();
        const auto record = std::find_if(m_objects.cbegin(), m_objects.cend(), [&](const ObjectRecord &value) { return value.id == selectedObject; });
        if (record == m_objects.cend()) return;
        const auto type = std::find_if(m_types.cbegin(), m_types.cend(), [&](const ObjectType &value) { return value.id == record->typeId; });
        if (type == m_types.cend()) return;
        for (const FieldDefinition &definition : type->fields) {
            if (definition.archived) continue;
            if (definition.fieldType == QStringLiteral("integer") || definition.fieldType == QStringLiteral("decimal") ||
                definition.fieldType == QStringLiteral("percentage") || definition.fieldType == QStringLiteral("progress"))
                field->addItem(definition.name, definition.id);
        }
        if (editing && selectedObject == original.objectId) {
            const int index = field->findData(original.fieldId);
            if (index >= 0) field->setCurrentIndex(index);
        }
    };
    connect(object, &QComboBox::currentIndexChanged, &dialog, [rebuildGoalFields] { rebuildGoalFields(); });
    rebuildGoalFields();
    bodyLayout->addWidget(fieldPanel);

    auto *taskPanel = panel(QStringLiteral("goalEditorTasks"), body);
    auto *taskLayout = new QVBoxLayout(taskPanel);
    taskLayout->setContentsMargins(7, 7, 7, 7);
    taskLayout->addWidget(textLabel(QStringLiteral("Checklist tasks (one task per line)")));
    auto *taskText = new QTextEdit(taskPanel);
    taskText->setObjectName(QStringLiteral("goalTasksInput"));
    taskText->setPlaceholderText(QStringLiteral("Complete daily commission\nSpend resin\nClaim reward"));
    taskText->setMinimumHeight(110);
    if (editing) {
        QStringList titles;
        for (const GoalTask &task : original.tasks) titles.append(task.title);
        taskText->setPlainText(titles.join(QLatin1Char('\n')));
    }
    taskLayout->addWidget(taskText);
    bodyLayout->addWidget(taskPanel);

    auto *dependencyPanel = panel(QStringLiteral("goalEditorDependencies"), body);
    auto *dependencyLayout = new QVBoxLayout(dependencyPanel);
    dependencyLayout->setContentsMargins(7, 7, 7, 7);
    dependencyLayout->addWidget(textLabel(QStringLiteral("Prerequisite goals")));
    auto *prerequisites = new QListWidget(dependencyPanel);
    prerequisites->setObjectName(QStringLiteral("goalPrerequisitesInput"));
    prerequisites->setMaximumHeight(150);
    for (const Goal &candidate : std::as_const(m_goals)) {
        if (candidate.id == goalId) continue;
        auto *item = new QListWidgetItem(candidate.title, prerequisites);
        item->setData(Qt::UserRole, candidate.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(original.prerequisiteIds.contains(candidate.id) ? Qt::Checked : Qt::Unchecked);
    }
    dependencyLayout->addWidget(prerequisites);
    bodyLayout->addWidget(dependencyPanel);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    auto updateVisibility = [kind, numericPanel, fieldPanel, taskPanel, target, current, currentLabel] {
        const QString selected = kind->currentData().toString();
        const bool numeric = selected == QStringLiteral("numeric");
        const bool fieldTarget = selected == QStringLiteral("field_target");
        numericPanel->setVisible(numeric || fieldTarget);
        current->setVisible(numeric);
        if (currentLabel) currentLabel->setVisible(numeric);
        fieldPanel->setVisible(fieldTarget);
        taskPanel->setVisible(selected == QStringLiteral("checklist"));
    };
    connect(kind, &QComboBox::currentIndexChanged, &dialog, [updateVisibility] { updateVisibility(); });
    updateVisibility();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *save = buttons->addButton(editing ? QStringLiteral("Save Goal") : QStringLiteral("Create Goal"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("goldButton"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [this, &dialog, editing, original, title, description, kind, target,
        current, object, field, taskText, prerequisites] {
        if (title->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Goal title required"), QStringLiteral("Enter a title for this goal."));
            title->setFocus();
            return;
        }
        Goal goal = original;
        goal.gameId = m_selectedGameId;
        goal.title = title->text().trimmed();
        goal.description = description->toPlainText().trimmed();
        goal.goalType = kind->currentData().toString();
        goal.prerequisiteIds.clear();
        for (int index = 0; index < prerequisites->count(); ++index) {
            const QListWidgetItem *item = prerequisites->item(index);
            if (item->checkState() == Qt::Checked) goal.prerequisiteIds.append(item->data(Qt::UserRole).toString());
        }
        QVector<GoalTask> tasks;
        if (goal.goalType == QStringLiteral("numeric")) {
            goal.target = target->value();
            goal.current = current->value();
        } else if (goal.goalType == QStringLiteral("field_target")) {
            goal.target = target->value();
            goal.objectId = object->currentData().toString();
            goal.fieldId = field->currentData().toString();
        } else if (goal.goalType == QStringLiteral("checklist")) {
            const QStringList lines = taskText->toPlainText().split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const QString taskTitle = line.trimmed();
                if (taskTitle.isEmpty()) continue;
                GoalTask task;
                if (editing && original.goalType == QStringLiteral("checklist") && tasks.size() < original.tasks.size())
                    task = original.tasks.at(tasks.size());
                task.title = taskTitle;
                task.sortOrder = tasks.size();
                tasks.append(task);
            }
        }
        QString error;
        if (!m_repository.saveGoal(goal, tasks, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not save goal"), error);
            return;
        }
        dialog.accept();
    });
    title->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        selectPage(Page::Goals);
        refreshGames(m_selectedGameId);
    }
}

QString MainWindow::openAssetLibrary(QWidget *parent, const QString &currentReference, bool selectionMode)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(selectionMode ? QStringLiteral("Choose Image") : QStringLiteral("Uploaded Images"));
    dialog.setMinimumSize(560, 440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *summary = textLabel(QStringLiteral("Loading image library…"), {}, &dialog);
    layout->addWidget(summary);
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("uploadedAssetsList"));
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(72, 72));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list, 1);
    auto *actions = new QHBoxLayout();
    auto *upload = button(QStringLiteral("Upload Image"), QStringLiteral("goldButton"), &dialog);
    upload->setObjectName(QStringLiteral("uploadAssetButton"));
    auto *remove = button(QStringLiteral("Delete"), QStringLiteral("softButton"), &dialog);
    auto *retry = button(QStringLiteral("Retry Cleanup"), QStringLiteral("softButton"), &dialog);
    actions->addWidget(upload);
    actions->addWidget(remove);
    actions->addWidget(retry);
    actions->addStretch(1);
    QPushButton *choose = nullptr;
    if (selectionMode) {
        choose = button(QStringLiteral("Use Selected"), QStringLiteral("goldButton"), &dialog);
        choose->setObjectName(QStringLiteral("useSelectedAssetButton"));
        choose->setEnabled(false);
        actions->addWidget(choose);
    }
    auto *close = button(selectionMode ? QStringLiteral("Cancel") : QStringLiteral("Close"), QStringLiteral("softButton"), &dialog);
    actions->addWidget(close);
    layout->addLayout(actions);

    auto refresh = [this, list, summary, currentReference] {
        list->clear();
        QString error;
        const QVector<UploadedAsset> assets = m_repository.uploadedAssets(&error);
        if (!error.isEmpty()) summary->setText(QStringLiteral("Image library warning: %1").arg(error));
        else summary->setText(QStringLiteral("%1 images · quota: 1000 files / 250 MB. Pending deletions remain counted until cleanup succeeds.").arg(assets.size()));
        for (const UploadedAsset &asset : assets) {
            auto *item = new QListWidgetItem(QStringLiteral("%1\n%2 · %3")
                .arg(asset.pendingDeletion ? QStringLiteral("Pending deletion") : asset.id.left(8),
                     QString::number(asset.sizeBytes / 1024.0, 'f', 1) + QStringLiteral(" KB"),
                     asset.pendingDeletion ? QStringLiteral("cleanup queued") : QStringLiteral("PNG")), list);
            item->setData(Qt::UserRole, asset.reference);
            item->setData(Qt::UserRole + 1, asset.id);
            item->setData(Qt::UserRole + 2, asset.pendingDeletion);
            item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
            if (!asset.pendingDeletion) {
                const QImage image = m_repository.loadAssetImage(asset.reference);
                if (!image.isNull()) item->setIcon(QIcon(QPixmap::fromImage(image)));
            }
            item->setToolTip(asset.reference);
            if (asset.reference == currentReference) list->setCurrentItem(item);
        }
    };
    refresh();
    if (choose) connect(list, &QListWidget::currentItemChanged, &dialog, [choose](QListWidgetItem *current) {
        choose->setEnabled(current && !current->data(Qt::UserRole + 2).toBool());
    });
    if (choose && list->currentItem())
        choose->setEnabled(!list->currentItem()->data(Qt::UserRole + 2).toBool());
    connect(upload, &QPushButton::clicked, &dialog, [this, &dialog, refresh] {
        const QString formats = ImageDecoder::supportsWebp()
            ? QStringLiteral("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)")
            : QStringLiteral("Images (*.png *.jpg *.jpeg *.gif *.bmp)");
        const QString filename = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Upload Image"), {}, formats);
        if (filename.isEmpty()) return;
        QFile file(filename);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not read image"), file.errorString());
            return;
        }
        const QByteArray encoded = file.read(5 * 1024 * 1024 + 1);
        if (file.error() != QFileDevice::NoError) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not read image"), file.errorString());
            return;
        }
        UploadedAsset created;
        QString error;
        if (!m_repository.uploadAsset(encoded, &created, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not upload image"), error);
            return;
        }
        refresh();
    });
    connect(remove, &QPushButton::clicked, &dialog, [this, &dialog, list, refresh] {
        QListWidgetItem *item = list->currentItem();
        if (!item) return;
        if (QMessageBox::question(&dialog, QStringLiteral("Delete image"),
            QStringLiteral("Delete this image? Images used by active or archived records are protected.")) != QMessageBox::Yes) return;
        bool pending = false;
        QString error;
        if (!m_repository.deleteAsset(item->data(Qt::UserRole + 1).toString(), &pending, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not delete image"), error);
            return;
        }
        if (pending) QMessageBox::information(&dialog, QStringLiteral("Cleanup queued"), error);
        refresh();
    });
    connect(retry, &QPushButton::clicked, &dialog, [this, &dialog, refresh] {
        QString error;
        if (!m_repository.retryAssetCleanup(&error)) QMessageBox::warning(&dialog, QStringLiteral("Cleanup still pending"), error);
        refresh();
    });
    QString selected;
    if (choose) connect(choose, &QPushButton::clicked, &dialog, [&dialog, list, &selected] {
        if (QListWidgetItem *item = list->currentItem()) {
            if (!item->data(Qt::UserRole + 2).toBool()) {
                selected = item->data(Qt::UserRole).toString();
                dialog.accept();
            }
        }
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.exec();
    return selected;
}

void MainWindow::openProfile()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("profileSettingsDialog"));
    dialog.setWindowTitle(QStringLiteral("Profile & Settings"));
    dialog.setMinimumWidth(560);
    dialog.setMaximumWidth(640);
    dialog.resize(640, 760);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = panel(QStringLiteral("settingsHeader"));
    header->setStyleSheet(QStringLiteral("QFrame#settingsHeader { background:#4d3768;border:0;border-bottom:3px solid #b795c7; }"));
    header->setFixedHeight(55);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 7, 14, 7);
    headerLayout->addWidget(textLabel(QStringLiteral("PROFILE & SETTINGS"), QStringLiteral("pixelHeading")));
    headerLayout->addStretch(1);
    auto *close = button(QStringLiteral("×"), QStringLiteral("iconButton"), header);
    close->setObjectName(QStringLiteral("dialogClose"));
    close->setStyleSheet(QStringLiteral("QPushButton#dialogClose { background:transparent;border:0;font-size:24px;min-width:30px;max-width:30px;min-height:30px;max-height:30px;padding:0; }"));
    headerLayout->addWidget(close);
    layout->addWidget(header);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);

    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { border:0;background:#fff2d6; }"));
    auto *body = new QWidget(scroll);
    body->setStyleSheet(QStringLiteral("background:#fff2d6;"));
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(15, 14, 15, 14);
    bodyLayout->setSpacing(7);
    auto addSection = [bodyLayout](const QString &title, const QString &bodyText, bool emphasize = false) {
        auto *box = panel(QStringLiteral(""));
        box->setObjectName(QStringLiteral("settingsSection"));
        box->setStyleSheet(QStringLiteral("QFrame#settingsSection { background:#fffaf2; border:1px solid #d8c9b9; }"));
        auto *boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(11, 9, 11, 9);
        boxLayout->setSpacing(5);
        auto *heading = textLabel(title.toUpper());
        heading->setStyleSheet(QStringLiteral("color:#49334e;font-size:13px;font-weight:800;"));
        boxLayout->addWidget(heading);
        auto *description = textLabel(bodyText);
        description->setStyleSheet(emphasize ? QStringLiteral("color:#49334e;font-weight:700;") : QStringLiteral("color:#715c74;font-size:12px;"));
        boxLayout->addWidget(description);
        bodyLayout->addWidget(box);
    };
    addSection(QStringLiteral("Account"), QStringLiteral("Username:  %1\nYour account and game data are stored in the local SQLite database.").arg(m_repository.activeUsername()), true);
    auto *securityBox = panel(QStringLiteral(""));
    securityBox->setObjectName(QStringLiteral("settingsSection"));
    securityBox->setStyleSheet(QStringLiteral("QFrame#settingsSection { background:#fffaf2; border:1px solid #d8c9b9; }"));
    auto *securityLayout = new QVBoxLayout(securityBox);
    securityLayout->setContentsMargins(11, 9, 11, 9);
    securityLayout->setSpacing(5);
    securityLayout->addWidget(textLabel(QStringLiteral("SECURITY")));
    securityLayout->addWidget(textLabel(QStringLiteral("Passwords are stored as bcrypt hashes. This desktop session ends when you lock or switch accounts.")));
    auto *changePassword = button(QStringLiteral("Change Password"), QStringLiteral("changePasswordButton"), securityBox);
    auto *lockAccount = button(QStringLiteral("Lock / Switch Account"), QStringLiteral("lockAccountButton"), securityBox);
    auto *importWebData = button(QStringLiteral("Import Existing Web Data"), QStringLiteral("importWebDataButton"), securityBox);
    securityLayout->addWidget(changePassword, 0, Qt::AlignLeft);
    securityLayout->addWidget(importWebData, 0, Qt::AlignLeft);
    securityLayout->addWidget(lockAccount, 0, Qt::AlignLeft);
    bodyLayout->addWidget(securityBox);
    connect(changePassword, &QPushButton::clicked, &dialog, [this, &dialog] {
        QDialog passwordDialog(&dialog);
        passwordDialog.setWindowTitle(QStringLiteral("Change Password"));
        auto *form = new QFormLayout(&passwordDialog);
        auto *current = new QLineEdit(&passwordDialog);
        current->setObjectName(QStringLiteral("currentPasswordInput"));
        current->setEchoMode(QLineEdit::Password);
        current->setMaxLength(72);
        auto *next = new QLineEdit(&passwordDialog);
        next->setObjectName(QStringLiteral("newPasswordInput"));
        next->setEchoMode(QLineEdit::Password);
        next->setMaxLength(72);
        auto *confirm = new QLineEdit(&passwordDialog);
        confirm->setObjectName(QStringLiteral("confirmPasswordInput"));
        confirm->setEchoMode(QLineEdit::Password);
        confirm->setMaxLength(72);
        form->addRow(QStringLiteral("Current password"), current);
        form->addRow(QStringLiteral("New password"), next);
        form->addRow(QStringLiteral("Confirm new password"), confirm);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &passwordDialog);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &passwordDialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &passwordDialog, [&] {
            if (next->text() != confirm->text()) {
                QMessageBox::warning(&passwordDialog, QStringLiteral("Password mismatch"), QStringLiteral("The password confirmation does not match."));
                return;
            }
            QString error;
            if (!m_repository.changePassword(current->text(), next->text(), &error)) {
                QMessageBox::warning(&passwordDialog, QStringLiteral("Could not change password"), error);
                return;
            }
            passwordDialog.accept();
        });
        passwordDialog.exec();
    });
    connect(lockAccount, &QPushButton::clicked, &dialog, [this, &dialog] {
        dialog.accept();
        emit accountLockRequested();
    });
    connect(importWebData, &QPushButton::clicked, &dialog, [this, &dialog] {
        WebImportDialog importDialog(m_repository, &dialog);
        if (importDialog.exec() == QDialog::Accepted) refreshGames(m_selectedGameId);
    });
    QString archiveError;
    const QVector<ArchivedEntry> archivedEntries = m_repository.archivedEntries(&archiveError);
    if (!archiveError.isEmpty()) {
        addSection(QStringLiteral("Archived Data"), archiveError);
    } else if (archivedEntries.isEmpty()) {
        addSection(QStringLiteral("Archived Data"), QStringLiteral("There is no archived data to restore right now."));
    } else {
        auto *box = panel(QStringLiteral(""));
        box->setObjectName(QStringLiteral("settingsSection"));
        box->setStyleSheet(QStringLiteral("QFrame#settingsSection { background:#fffaf2; border:1px solid #d8c9b9; }"));
        auto *boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(11, 9, 11, 9);
        boxLayout->setSpacing(5);
        auto *heading = textLabel(QStringLiteral("ARCHIVED DATA"));
        heading->setStyleSheet(QStringLiteral("color:#49334e;font-size:13px;font-weight:800;"));
        boxLayout->addWidget(heading);
        auto *description = textLabel(QStringLiteral("Restore games, sections, or objects from active games."));
        description->setStyleSheet(QStringLiteral("color:#715c74;font-size:12px;"));
        boxLayout->addWidget(description);
        for (const ArchivedEntry &entry : archivedEntries) {
            auto *row = new QWidget(box);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 2, 0, 2);
            const QString itemLabel = entry.kind == QStringLiteral("Game") ? entry.name
                : QStringLiteral("%1 · %2 · %3").arg(entry.kind, entry.gameName, entry.name);
            rowLayout->addWidget(textLabel(itemLabel), 1);
            if (entry.kind == QStringLiteral("Object")) {
                auto *history = button(QStringLiteral("History"), QStringLiteral("objectHistoryButton"), row);
                rowLayout->addWidget(history);
                connect(history, &QPushButton::clicked, &dialog, [this, &dialog, entry] {
                    showObjectHistory(&dialog, m_repository, entry.id, entry.name);
                });
            }
            auto *restore = button(QStringLiteral("Restore"), QStringLiteral("softButton"), row);
            restore->setObjectName(QStringLiteral("restoreArchivedButton"));
            rowLayout->addWidget(restore);
            boxLayout->addWidget(row);
            connect(restore, &QPushButton::clicked, &dialog, [this, &dialog, entry] {
                QString error;
                if (!m_repository.restoreArchivedEntry(entry, &error)) {
                    QMessageBox::warning(&dialog, QStringLiteral("Could not restore item"), error);
                    return;
                }
                dialog.accept();
                refreshGames(m_selectedGameId);
            });
        }
        bodyLayout->addWidget(box);
    }
    auto *imageBox = panel(QStringLiteral(""));
    imageBox->setObjectName(QStringLiteral("settingsSection"));
    imageBox->setStyleSheet(QStringLiteral("QFrame#settingsSection { background:#fffaf2; border:1px solid #d8c9b9; }"));
    auto *imageLayout = new QVBoxLayout(imageBox);
    imageLayout->setContentsMargins(11, 9, 11, 9);
    imageLayout->addWidget(textLabel(QStringLiteral("UPLOADED IMAGES")));
    imageLayout->addWidget(textLabel(QStringLiteral("Manage image storage, quota, and pending cleanup. Images referenced by archived records stay protected.")));
    auto *manageImages = button(QStringLiteral("Manage Images"), QStringLiteral("softButton"), imageBox);
    imageLayout->addWidget(manageImages, 0, Qt::AlignLeft);
    bodyLayout->addWidget(imageBox);
    connect(manageImages, &QPushButton::clicked, &dialog, [this, &dialog] { openAssetLibrary(&dialog, {}, false); });
    bodyLayout->addStretch(1);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);

    auto *footer = panel(QStringLiteral("settingsFooter"));
    footer->setStyleSheet(QStringLiteral("QFrame#settingsFooter { background:#fff2d6;border:0;border-top:1px solid #e5d4ad; }"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 9, 12, 9);
    footerLayout->addStretch(1);
    auto *cancel = button(QStringLiteral("CANCEL"), QStringLiteral("softButton"), footer);
    auto *save = button(QStringLiteral("SAVE SECURITY SETTINGS"), QStringLiteral("goldButton"), footer);
    save->setEnabled(false);
    footerLayout->addWidget(cancel);
    footerLayout->addWidget(save);
    layout->addWidget(footer);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.exec();
}

} // namespace gmst
