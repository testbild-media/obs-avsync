// Qt dock "AV-Sync" (compact): clock + status line, phone QR codes (auto-collapsing),
// measurement table, A/V offset with graph, one-line session control, collapsible settings.
#include "plugin.hpp"
#include "qrcodegen.hpp"

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimeEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace avs {

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString ms(double v, int decimals = 1)
{
	if (!std::isfinite(v))
		return QStringLiteral("–");
	return QLocale().toString(v, 'f', decimals);
}

QString signedMs(double v, int decimals = 1)
{
	if (!std::isfinite(v))
		return QStringLiteral("–");
	QString s = QLocale().toString(std::fabs(v), 'f', decimals);
	return (v > 0 ? QStringLiteral("+") : v < 0 ? QStringLiteral("−") : QStringLiteral("±")) + s;
}

QPixmap makeQr(const QString &text, int targetPx)
{
	using qrcodegen::QrCode;
	QrCode qr = QrCode::encodeText(text.toUtf8().constData(), QrCode::Ecc::MEDIUM);
	const int border = 2;
	const int n = qr.getSize() + 2 * border;
	QImage img(n, n, QImage::Format_RGB32);
	img.fill(Qt::white);
	for (int y = 0; y < qr.getSize(); y++)
		for (int x = 0; x < qr.getSize(); x++)
			if (qr.getModule(x, y))
				img.setPixel(x + border, y + border, qRgb(0, 0, 0));
	const int scale = std::max(1, targetPx / n);
	return QPixmap::fromImage(img.scaled(n * scale, n * scale, Qt::IgnoreAspectRatio, Qt::FastTransformation));
}

QIcon dotIcon(const QColor &c)
{
	QPixmap pm(10, 10);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(c);
	p.drawEllipse(QRectF(1, 1, 8, 8));
	return QIcon(pm);
}

QLabel *sectionTitle(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text, parent);
	l->setStyleSheet(QStringLiteral("font-weight: bold;"));
	return l;
}

QFrame *separator(QWidget *parent)
{
	auto *f = new QFrame(parent);
	f->setFrameShape(QFrame::NoFrame);
	f->setFixedHeight(1);
	f->setStyleSheet(QStringLiteral("background-color: rgba(128, 128, 128, 70); border: none;"));
	return f;
}

QToolButton *toolButton(const QString &text, const QString &tip, QWidget *parent)
{
	auto *b = new QToolButton(parent);
	b->setText(text);
	b->setToolTip(tip);
	b->setAutoRaise(true);
	return b;
}

const QColor kGreen(62, 207, 142), kOrange(245, 185, 66), kGrey(120, 125, 135);
const QColor kVideoColor(77, 163, 255), kAudioColor(245, 160, 60), kOffsetColor(80, 200, 120);

// ---------------------------------------------------------------- latency graph

class LatencyGraph : public QWidget {
public:
	struct Series {
		QColor color;
		std::vector<std::pair<int64_t, double>> points; // seq, ms
	};

	explicit LatencyGraph(QWidget *parent = nullptr) : QWidget(parent)
	{
		setFixedHeight(64);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	void setSeries(std::vector<Series> s)
	{
		series_ = std::move(s);
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		QFont f = font();
		f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.75));
		p.setFont(f);
		const QRectF area = rect().adjusted(34, 3, -3, -3);
		p.fillRect(rect(), palette().base());

		int64_t s0 = INT64_MAX, s1 = INT64_MIN;
		double lo = 1e18, hi = -1e18;
		for (const auto &s : series_)
			for (const auto &pt : s.points) {
				s0 = std::min(s0, pt.first);
				s1 = std::max(s1, pt.first);
				lo = std::min(lo, pt.second);
				hi = std::max(hi, pt.second);
			}
		if (s0 > s1) {
			p.setPen(palette().color(QPalette::PlaceholderText));
			p.drawText(area, Qt::AlignCenter, T("Dock.Graph.Empty"));
			return;
		}
		if (hi - lo < 10.0) {
			const double mid = (hi + lo) / 2;
			lo = mid - 5;
			hi = mid + 5;
		}
		const double pad = (hi - lo) * 0.08;
		lo -= pad;
		hi += pad;
		if (s1 == s0)
			s1 = s0 + 1;
		auto X = [&](int64_t seq) { return area.left() + (double)(seq - s0) / (double)(s1 - s0) * area.width(); };
		auto Y = [&](double v) { return area.bottom() - (v - lo) / (hi - lo) * area.height(); };

		const double steps[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000};
		double step = steps[0];
		for (double st : steps) {
			step = st;
			if ((hi - lo) / st <= 3)
				break;
		}
		for (double v = std::ceil(lo / step) * step; v <= hi; v += step) {
			const double y = Y(v);
			p.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DotLine));
			p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
			p.setPen(palette().color(QPalette::PlaceholderText));
			p.drawText(QRectF(0, y - 7, area.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, ms(v, 0));
		}
		for (const auto &s : series_) {
			if (s.points.empty())
				continue;
			QPainterPath path;
			int64_t prev = 0;
			bool first = true;
			for (const auto &pt : s.points) {
				const QPointF q(X(pt.first), Y(pt.second));
				if (first || pt.first - prev > 3)
					path.moveTo(q);
				else
					path.lineTo(q);
				first = false;
				prev = pt.first;
			}
			p.setPen(QPen(s.color, 1.5));
			p.setBrush(Qt::NoBrush);
			p.drawPath(path);
		}
	}

private:
	std::vector<Series> series_;
};

// ---------------------------------------------------------------- dock widget

class AvSyncDock : public QWidget {
public:
	explicit AvSyncDock(QWidget *parent = nullptr);

private:
	QWidget *buildHeader();
	QWidget *buildPhonePanel();
	QWidget *buildMeasurements();
	QWidget *buildOffset();
	QWidget *buildSession();
	QWidget *buildSettings();

	void refresh();
	void refreshHeaderAndPhone(const std::vector<ProbeSnapshot> &snap);
	void refreshCombos(const std::vector<ProbeSnapshot> &snap);
	void refreshTable(const std::vector<ProbeSnapshot> &snap);
	void refreshOffset(const std::vector<ProbeSnapshot> &snap);
	void refreshSession();
	void loadSettingsIntoUi();
	void applySettings();
	void applyOffset();
	void startSession(bool now);

	QTimer timer_;
	QWidget *content_ = nullptr;

	// header
	QLabel *clockLabel_ = nullptr;
	QLabel *ntpLabel_ = nullptr;
	QToolButton *qrButton_ = nullptr;
	QToolButton *settingsButton_ = nullptr;

	// phone panel
	QWidget *phonePanel_ = nullptr;
	bool phonePanelUserChoice_ = false;
	QLabel *qrOnline_ = nullptr, *qrLan_ = nullptr;
	QWidget *qrOnlineCol_ = nullptr, *qrLanCol_ = nullptr;
	QString qrOnlineText_ = QStringLiteral("-"), qrLanText_ = QStringLiteral("-");
	QLabel *linksLabel_ = nullptr;
	QLabel *phonesLabel_ = nullptr;

	// measurements
	QTableWidget *table_ = nullptr;
	QLabel *emptyHint_ = nullptr;

	// offset
	QComboBox *videoCombo_ = nullptr;
	QComboBox *audioCombo_ = nullptr;
	std::vector<int> comboVideoIds_, comboAudioIds_;
	QLabel *offsetLabel_ = nullptr;
	QLabel *offsetHint_ = nullptr;
	QPushButton *applyButton_ = nullptr;
	LatencyGraph *graph_ = nullptr;

	// session
	QTimeEdit *startEdit_ = nullptr;
	QSpinBox *durationSpin_ = nullptr;
	QToolButton *cancelButton_ = nullptr;
	QLabel *sessionLabel_ = nullptr;

	// settings
	QWidget *settingsPanel_ = nullptr;
	QLineEdit *labelEdit_ = nullptr;
	QSpinBox *portSpin_ = nullptr;
	QCheckBox *serverCheck_ = nullptr;
	QCheckBox *ntpCheck_ = nullptr;
	QLineEdit *ntpEdit_ = nullptr;
	QCheckBox *progVideoCheck_ = nullptr;
	QCheckBox *progAudioCheck_ = nullptr;
	QSpinBox *trackSpin_ = nullptr;
	QSpinBox *maxLatSpin_ = nullptr;
	QSpinBox *windowSpin_ = nullptr;
	QLineEdit *csvEdit_ = nullptr;
	QCheckBox *csvGermanCheck_ = nullptr;
	QLineEdit *publicUrlEdit_ = nullptr;
};

AvSyncDock::AvSyncDock(QWidget *parent) : QWidget(parent)
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	outer->addWidget(scroll);

	content_ = new QWidget(scroll);
	auto *lay = new QVBoxLayout(content_);
	lay->setContentsMargins(6, 4, 6, 4);
	lay->setSpacing(4);
	lay->addWidget(buildHeader());
	lay->addWidget(buildPhonePanel());
	lay->addWidget(buildSettings());
	lay->addWidget(separator(content_));
	lay->addWidget(buildMeasurements());
	lay->addWidget(separator(content_));
	lay->addWidget(buildOffset());
	lay->addWidget(separator(content_));
	lay->addWidget(buildSession());
	lay->addStretch(1);
	scroll->setWidget(content_);

	loadSettingsIntoUi();
	connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
	timer_.start(200);
}

QWidget *AvSyncDock::buildHeader()
{
	auto *w = new QWidget(content_);
	auto *row = new QHBoxLayout(w);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(6);
	clockLabel_ = new QLabel(QStringLiteral("--:--:--.---"), w);
	// a widget style sheet wins over the OBS theme (setFont() alone does not)
	clockLabel_->setStyleSheet(QStringLiteral(
		"font-family: 'Consolas', 'Cascadia Mono', 'Menlo', monospace; font-size: 15pt; font-weight: bold;"));
	ntpLabel_ = new QLabel(w);
	qrButton_ = toolButton(QStringLiteral("📱"), T("Dock.Phone.Toggle"), w);
	auto *reset = toolButton(QStringLiteral("↺"), T("Dock.Reset"), w);
	settingsButton_ = toolButton(QStringLiteral("⚙"), T("Dock.Settings"), w);
	settingsButton_->setCheckable(true);
	connect(qrButton_, &QToolButton::clicked, this, [this] {
		phonePanelUserChoice_ = true;
		phonePanel_->setVisible(!phonePanel_->isVisible());
	});
	connect(reset, &QToolButton::clicked, this, [] { Plugin::instance().engine().resetStats(); });
	connect(settingsButton_, &QToolButton::toggled, this, [this](bool on) { settingsPanel_->setVisible(on); });
	row->addWidget(clockLabel_);
	row->addWidget(ntpLabel_);
	row->addStretch(1);
	row->addWidget(qrButton_);
	row->addWidget(reset);
	row->addWidget(settingsButton_);
	return w;
}

QWidget *AvSyncDock::buildPhonePanel()
{
	phonePanel_ = new QWidget(content_);
	auto *row = new QHBoxLayout(phonePanel_);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(8);
	auto addQr = [&](QLabel *&qr, QWidget *&column, const QString &caption) {
		column = new QWidget(phonePanel_);
		auto *col = new QVBoxLayout(column);
		col->setContentsMargins(0, 0, 0, 0);
		col->setSpacing(1);
		qr = new QLabel(column);
		qr->setFixedSize(92, 92);
		qr->setAlignment(Qt::AlignCenter);
		auto *cap = new QLabel(caption, column);
		cap->setAlignment(Qt::AlignCenter);
		cap->setStyleSheet(QStringLiteral("font-size: 8pt;"));
		col->addWidget(qr, 0, Qt::AlignHCenter);
		col->addWidget(cap);
		row->addWidget(column, 0, Qt::AlignTop);
	};
	addQr(qrOnline_, qrOnlineCol_, T("Dock.Phone.QrOnline"));
	addQr(qrLan_, qrLanCol_, T("Dock.Phone.QrLan"));

	auto *info = new QVBoxLayout();
	info->setSpacing(2);
	linksLabel_ = new QLabel(phonePanel_);
	linksLabel_->setWordWrap(true);
	linksLabel_->setTextFormat(Qt::RichText);
	linksLabel_->setOpenExternalLinks(true);
	linksLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
	linksLabel_->setStyleSheet(QStringLiteral("font-size: 8pt;"));
	phonesLabel_ = new QLabel(phonePanel_);
	phonesLabel_->setWordWrap(true);
	phonesLabel_->setStyleSheet(QStringLiteral("font-size: 8pt;"));
	info->addWidget(linksLabel_);
	info->addWidget(phonesLabel_);
	info->addStretch(1);
	row->addLayout(info, 1);
	return phonePanel_;
}

QWidget *AvSyncDock::buildMeasurements()
{
	auto *w = new QWidget(content_);
	auto *lay = new QVBoxLayout(w);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(2);
	table_ = new QTableWidget(0, 4, w);
	table_->setHorizontalHeaderLabels(
		{T("Dock.Col.Probe"), T("Dock.Col.Latency"), T("Dock.Col.Jitter"), T("Dock.Col.Count")});
	table_->verticalHeader()->setVisible(false);
	table_->verticalHeader()->setDefaultSectionSize(20);
	table_->setShowGrid(false);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->setSelectionMode(QAbstractItemView::NoSelection);
	table_->setFocusPolicy(Qt::NoFocus);
	table_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	table_->horizontalHeader()->setHighlightSections(false);
	table_->setToolTip(T("Dock.Table.Tip"));
	emptyHint_ = new QLabel(T("Dock.Measurements.Hint"), w);
	emptyHint_->setWordWrap(true);
	emptyHint_->setStyleSheet(QStringLiteral("font-size: 8pt;"));
	lay->addWidget(table_);
	lay->addWidget(emptyHint_);
	return w;
}

QWidget *AvSyncDock::buildOffset()
{
	auto *w = new QWidget(content_);
	auto *lay = new QVBoxLayout(w);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(3);

	auto *pairRow = new QHBoxLayout();
	pairRow->setSpacing(4);
	videoCombo_ = new QComboBox(w);
	audioCombo_ = new QComboBox(w);
	videoCombo_->setToolTip(T("Dock.Offset.Video"));
	audioCombo_->setToolTip(T("Dock.Offset.Audio"));
	videoCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	audioCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	videoCombo_->setMinimumContentsLength(6);
	audioCombo_->setMinimumContentsLength(6);
	pairRow->addWidget(new QLabel(QStringLiteral("🎥"), w));
	pairRow->addWidget(videoCombo_, 1);
	pairRow->addWidget(new QLabel(QStringLiteral("🎤"), w));
	pairRow->addWidget(audioCombo_, 1);
	lay->addLayout(pairRow);

	auto pairChanged = [this] {
		const int vi = videoCombo_->currentIndex(), ai = audioCombo_->currentIndex();
		const int v = vi >= 0 && vi < (int)comboVideoIds_.size() ? comboVideoIds_[(size_t)vi] : -1;
		const int a = ai >= 0 && ai < (int)comboAudioIds_.size() ? comboAudioIds_[(size_t)ai] : -1;
		Plugin::instance().setPair(v, a);
	};
	connect(videoCombo_, QOverload<int>::of(&QComboBox::activated), this, pairChanged);
	connect(audioCombo_, QOverload<int>::of(&QComboBox::activated), this, pairChanged);

	auto *valueRow = new QHBoxLayout();
	valueRow->setSpacing(6);
	offsetLabel_ = new QLabel(QStringLiteral("Δ –"), w);
	offsetLabel_->setStyleSheet(QStringLiteral("font-size: 15pt; font-weight: bold;"));
	offsetHint_ = new QLabel(w);
	offsetHint_->setWordWrap(true);
	applyButton_ = new QPushButton(T("Dock.Offset.Apply"), w);
	applyButton_->setToolTip(T("Dock.Offset.Apply.Tip"));
	connect(applyButton_, &QPushButton::clicked, this, [this] { applyOffset(); });
	valueRow->addWidget(offsetLabel_);
	valueRow->addWidget(offsetHint_, 1);
	valueRow->addWidget(applyButton_);
	lay->addLayout(valueRow);

	graph_ = new LatencyGraph(w);
	graph_->setToolTip(T("Dock.Graph.Tip"));
	lay->addWidget(graph_);
	return w;
}

QWidget *AvSyncDock::buildSession()
{
	auto *w = new QWidget(content_);
	auto *lay = new QVBoxLayout(w);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(2);
	auto *row = new QHBoxLayout();
	row->setSpacing(3);
	startEdit_ = new QTimeEdit(w);
	startEdit_->setDisplayFormat(QStringLiteral("HH:mm:ss"));
	startEdit_->setTime(QTime::currentTime().addSecs(60));
	startEdit_->setToolTip(T("Dock.Session.Start"));
	durationSpin_ = new QSpinBox(w);
	durationSpin_->setRange(5, 24 * 3600);
	durationSpin_->setSuffix(QStringLiteral(" s"));
	durationSpin_->setToolTip(T("Dock.Session.Duration"));
	auto *plan = new QPushButton(T("Dock.Session.Schedule"), w);
	plan->setToolTip(T("Dock.Session.Schedule.Tip"));
	auto *now = new QPushButton(T("Dock.Session.Now"), w);
	now->setToolTip(T("Dock.Session.Now.Tip"));
	cancelButton_ = toolButton(QStringLiteral("✕"), T("Dock.Session.Cancel"), w);
	auto *folder = toolButton(QStringLiteral("📁"), T("Dock.Session.OpenFolder"), w);
	connect(plan, &QPushButton::clicked, this, [this] { startSession(false); });
	connect(now, &QPushButton::clicked, this, [this] { startSession(true); });
	connect(cancelButton_, &QToolButton::clicked, this, [] { Plugin::instance().engine().cancelSession(); });
	connect(folder, &QToolButton::clicked, this, [] {
		const QString dir = QString::fromStdString(Plugin::instance().config().csvFolder);
		QDir().mkpath(dir);
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	});
	row->addWidget(sectionTitle(T("Dock.Session"), w));
	row->addWidget(startEdit_);
	row->addWidget(durationSpin_);
	row->addWidget(plan);
	row->addWidget(now);
	row->addWidget(cancelButton_);
	row->addWidget(folder);
	row->addStretch(1);
	lay->addLayout(row);

	sessionLabel_ = new QLabel(w);
	sessionLabel_->setWordWrap(true);
	sessionLabel_->setTextFormat(Qt::RichText);
	sessionLabel_->setStyleSheet(QStringLiteral("font-size: 8pt;"));
	connect(sessionLabel_, &QLabel::linkActivated, this,
		[](const QString &link) { QDesktopServices::openUrl(QUrl::fromLocalFile(link)); });
	lay->addWidget(sessionLabel_);
	return w;
}

QWidget *AvSyncDock::buildSettings()
{
	settingsPanel_ = new QFrame(content_);
	static_cast<QFrame *>(settingsPanel_)->setFrameShape(QFrame::StyledPanel);
	settingsPanel_->setVisible(false);
	auto *form = new QFormLayout(settingsPanel_);
	form->setContentsMargins(6, 4, 6, 4);
	form->setVerticalSpacing(3);

	labelEdit_ = new QLineEdit(settingsPanel_);
	labelEdit_->setPlaceholderText(T("Dock.Session.LabelPlaceholder"));
	serverCheck_ = new QCheckBox(T("Dock.Settings.Server"), settingsPanel_);
	portSpin_ = new QSpinBox(settingsPanel_);
	portSpin_->setRange(1024, 65535);
	publicUrlEdit_ = new QLineEdit(settingsPanel_);
	publicUrlEdit_->setPlaceholderText(QStringLiteral("https://testbild-media.github.io/obs-avsync/"));
	ntpCheck_ = new QCheckBox(T("Dock.Settings.Ntp"), settingsPanel_);
	ntpEdit_ = new QLineEdit(settingsPanel_);
	progVideoCheck_ = new QCheckBox(T("Dock.Settings.ProgramVideo"), settingsPanel_);
	progAudioCheck_ = new QCheckBox(T("Dock.Settings.ProgramAudio"), settingsPanel_);
	trackSpin_ = new QSpinBox(settingsPanel_);
	trackSpin_->setRange(1, 6);
	trackSpin_->setToolTip(T("Dock.Settings.Track"));
	maxLatSpin_ = new QSpinBox(settingsPanel_);
	maxLatSpin_->setRange(200, 9000);
	maxLatSpin_->setSingleStep(100);
	maxLatSpin_->setSuffix(QStringLiteral(" ms"));
	maxLatSpin_->setToolTip(T("Dock.Settings.MaxLatency.Tip"));
	windowSpin_ = new QSpinBox(settingsPanel_);
	windowSpin_->setRange(3, 600);
	csvEdit_ = new QLineEdit(settingsPanel_);
	auto *browse = new QToolButton(settingsPanel_);
	browse->setText(QStringLiteral("…"));
	connect(browse, &QToolButton::clicked, this, [this] {
		const QString dir = QFileDialog::getExistingDirectory(this, T("Dock.Settings.CsvFolder"), csvEdit_->text());
		if (!dir.isEmpty())
			csvEdit_->setText(QDir::toNativeSeparators(dir));
	});
	auto *csvRow = new QHBoxLayout();
	csvRow->addWidget(csvEdit_, 1);
	csvRow->addWidget(browse);
	csvGermanCheck_ = new QCheckBox(T("Dock.Settings.CsvGerman"), settingsPanel_);

	auto *progRow = new QHBoxLayout();
	progRow->addWidget(progVideoCheck_);
	progRow->addWidget(progAudioCheck_);
	progRow->addWidget(trackSpin_);
	progRow->addStretch(1);
	auto *serverRow = new QHBoxLayout();
	serverRow->addWidget(serverCheck_);
	serverRow->addWidget(portSpin_);
	serverRow->addStretch(1);

	form->addRow(T("Dock.Session.Label"), labelEdit_);
	form->addRow(serverRow);
	form->addRow(T("Dock.Settings.PublicUrl"), publicUrlEdit_);
	form->addRow(ntpCheck_);
	form->addRow(T("Dock.Settings.NtpServers"), ntpEdit_);
	form->addRow(progRow);
	form->addRow(T("Dock.Settings.MaxLatency"), maxLatSpin_);
	form->addRow(T("Dock.Settings.Window"), windowSpin_);
	form->addRow(T("Dock.Settings.CsvFolder"), csvRow);
	form->addRow(csvGermanCheck_);
	auto *apply = new QPushButton(T("Dock.Settings.Apply"), settingsPanel_);
	connect(apply, &QPushButton::clicked, this, [this] { applySettings(); });
	form->addRow(apply);
	return settingsPanel_;
}

void AvSyncDock::loadSettingsIntoUi()
{
	const Config c = Plugin::instance().config();
	serverCheck_->setChecked(c.serverEnabled);
	portSpin_->setValue(c.port);
	ntpCheck_->setChecked(c.ntpEnabled);
	ntpEdit_->setText(QString::fromStdString(c.ntpServers));
	progVideoCheck_->setChecked(c.programVideo);
	progAudioCheck_->setChecked(c.programAudio);
	trackSpin_->setValue(c.programTrack);
	maxLatSpin_->setValue((int)c.maxLatencyMs);
	windowSpin_->setValue(c.liveWindow);
	csvEdit_->setText(QString::fromStdString(c.csvFolder));
	csvGermanCheck_->setChecked(c.csvGerman);
	publicUrlEdit_->setText(QString::fromStdString(c.publicPageUrl));
	durationSpin_->setValue(c.sessionDurationS);
}

void AvSyncDock::applySettings()
{
	Config c = Plugin::instance().config();
	c.serverEnabled = serverCheck_->isChecked();
	c.port = portSpin_->value();
	c.ntpEnabled = ntpCheck_->isChecked();
	c.ntpServers = ntpEdit_->text().toStdString();
	c.programVideo = progVideoCheck_->isChecked();
	c.programAudio = progAudioCheck_->isChecked();
	c.programTrack = trackSpin_->value();
	c.maxLatencyMs = maxLatSpin_->value();
	c.liveWindow = windowSpin_->value();
	c.csvFolder = csvEdit_->text().toStdString();
	c.csvGerman = csvGermanCheck_->isChecked();
	c.publicPageUrl = publicUrlEdit_->text().trimmed().toStdString();
	Plugin::instance().applyConfig(c);
	qrOnlineText_ = qrLanText_ = QStringLiteral("-");
	settingsButton_->setChecked(false);
}

void AvSyncDock::startSession(bool now)
{
	Plugin &p = Plugin::instance();
	double start = 0.0; // next full second
	if (!now) {
		const double utcNow = p.nowUtcMs();
		const QDateTime nowLocal = QDateTime::fromMSecsSinceEpoch((qint64)utcNow);
		QDateTime dt(nowLocal.date(), startEdit_->time());
		if (dt.toMSecsSinceEpoch() <= (qint64)utcNow)
			dt = dt.addDays(1);
		start = (double)dt.toMSecsSinceEpoch();
	}
	p.startSession(start, durationSpin_->value() * 1000.0, labelEdit_->text().toStdString());
	Config c = p.config();
	if (c.sessionDurationS != durationSpin_->value()) {
		c.sessionDurationS = durationSpin_->value();
		p.applyConfig(c);
	}
}

void AvSyncDock::applyOffset()
{
	Plugin &p = Plugin::instance();
	int v, a;
	p.resolvedPair(v, a);
	const auto snap = p.engine().snapshot(0);
	const ProbeSnapshot *audio = nullptr;
	for (const auto &s : snap)
		if (s.id == a)
			audio = &s;
	if (!audio || audio->sourceUuid.empty())
		return;
	const PairStats ps = p.engine().pair(v, a, (size_t)p.config().liveWindow);
	if (ps.offset.count < 3)
		return;

	obs_source_t *src = obs_get_source_by_uuid(audio->sourceUuid.c_str());
	if (!src)
		return;
	const int64_t current = obs_source_get_sync_offset(src);
	// offset = audio latency - video latency -> shift the audio by -offset
	const int64_t proposed = current - (int64_t)std::llround(ps.offset.median * 1e6);
	const QString text = T("Dock.Offset.Confirm")
				     .arg(QString::fromStdString(audio->name))
				     .arg(ms(current / 1e6, 0))
				     .arg(ms(proposed / 1e6, 0));
	if (QMessageBox::question(this, T("Dock.Offset.Apply"), text) == QMessageBox::Yes) {
		obs_source_set_sync_offset(src, proposed);
		p.engine().resetStats();
		blog(LOG_INFO, "[obs-avsync] sync offset of '%s' set to %.0f ms", audio->name.c_str(), proposed / 1e6);
	}
	obs_source_release(src);
}

void AvSyncDock::refresh()
{
	if (!isVisible())
		return;

	// test hook: AVSYNC_DOCK_SNAPSHOT=<file.png> saves a screenshot of the dock every 5 s
	static const char *snapshotPath = getenv("AVSYNC_DOCK_SNAPSHOT");
	static int snapshotCounter = 0;
	if (snapshotPath && ++snapshotCounter % 25 == 0) {
		// the theme paints the dark background on the dock frame, so render onto a dark canvas
		QPixmap pm(content_->size());
		pm.fill(QColor(0x1e, 0x1f, 0x29));
		content_->render(&pm, QPoint(), QRegion(), QWidget::DrawChildren);
		pm.save(QString::fromLocal8Bit(snapshotPath));
		if (snapshotCounter == 25) {
			phonePanel_->resize(content_->width(), phonePanel_->sizeHint().height());
			phonePanel_->grab().save(QString::fromLocal8Bit(snapshotPath) + QStringLiteral(".phone.png"));
		}
	}

	auto snap = Plugin::instance().engine().snapshot(60);
	std::stable_sort(snap.begin(), snap.end(), [](const ProbeSnapshot &a, const ProbeSnapshot &b) {
		if (a.kind != b.kind)
			return a.kind == Kind::Video;
		return a.origin == "filter" && b.origin != "filter";
	});
	refreshHeaderAndPhone(snap);
	refreshCombos(snap);
	refreshTable(snap);
	refreshOffset(snap);
	refreshSession();
}

void AvSyncDock::refreshHeaderAndPhone(const std::vector<ProbeSnapshot> &snap)
{
	Plugin &p = Plugin::instance();
	const double utc = p.nowUtcMs();
	clockLabel_->setText(QDateTime::fromMSecsSinceEpoch((qint64)utc).toString(QStringLiteral("HH:mm:ss.zzz")));

	const ClockStatus cs = p.clock().status();
	if (cs.ntpSynced) {
		ntpLabel_->setText(QStringLiteral("NTP ±%1 ms").arg(ms(cs.errorMs)));
		ntpLabel_->setStyleSheet(QStringLiteral("color: #3ecf8e; font-size: 8pt;"));
		ntpLabel_->setToolTip(T("Dock.Clock.Ntp").arg(QString::fromStdString(cs.server), ms(cs.errorMs),
								 signedMs(cs.systemOffsetMs, 0)));
	} else {
		ntpLabel_->setText(T("Dock.Clock.SystemShort"));
		ntpLabel_->setStyleSheet(QStringLiteral("color: #f5b942; font-size: 8pt;"));
		ntpLabel_->setToolTip(T("Dock.Clock.System").arg(QString::fromStdString(cs.lastError)));
	}

	// phone panel: online page (works everywhere) and LAN page served by the plugin (precise)
	const Config c = p.config();
	const QString online = QString::fromStdString(c.publicPageUrl);
	const auto urls = p.phoneUrls();
	const QString lan = urls.empty() ? QString() : QString::fromStdString(urls.front());
	QString html;
	if (!online.isEmpty())
		html += QStringLiteral("<a href=\"%1\">%1</a><br>").arg(online.toHtmlEscaped());
	for (const auto &u : urls)
		html += QStringLiteral("<a href=\"%1\">%1</a> ").arg(QString::fromStdString(u).toHtmlEscaped());
	if (urls.empty() && c.serverEnabled)
		html += QStringLiteral("<span style=\"color:#e05050\">%1</span>")
				.arg(T("Dock.Phone.ServerError").arg(QString::fromStdString(p.serverError()).toHtmlEscaped()));
	if (html != linksLabel_->text())
		linksLabel_->setText(html);
	if (online != qrOnlineText_) {
		qrOnlineText_ = online;
		qrOnline_->setPixmap(online.isEmpty() ? QPixmap() : makeQr(online, 92));
		qrOnlineCol_->setVisible(!online.isEmpty());
	}
	if (lan != qrLanText_) {
		qrLanText_ = lan;
		qrLan_->setPixmap(lan.isEmpty() ? QPixmap() : makeQr(lan, 92));
		qrLanCol_->setVisible(!lan.isEmpty());
	}

	const auto clients = p.server().clients();
	QStringList phones;
	for (const auto &cl : clients) {
		phones << T("Dock.Phone.Client")
				  .arg(QString::fromStdString(cl.name.empty() ? cl.address : cl.name).toHtmlEscaped(),
				       ms(json::number(cl.status, "syncErrMs", NAN)),
				       ms(json::number(cl.status, "refreshHz", NAN), 0),
				       json::boolean(cl.status, "running") ? T("Dock.Phone.Running") : T("Dock.Phone.Paused"));
	}
	phonesLabel_->setText(phones.join(QStringLiteral("<br>")));
	phonesLabel_->setVisible(!phones.isEmpty());
	qrButton_->setText(clients.empty() ? QStringLiteral("📱") : QStringLiteral("📱 %1").arg(clients.size()));

	// Show the QR codes while nothing is measured, hide them as soon as results come in.
	if (!phonePanelUserChoice_) {
		const bool measuring = std::any_of(snap.begin(), snap.end(), [](const ProbeSnapshot &s) {
			return s.lastAgeMs >= 0 && s.lastAgeMs < 10000;
		});
		phonePanel_->setVisible(!measuring);
	}
}

void AvSyncDock::refreshCombos(const std::vector<ProbeSnapshot> &snap)
{
	std::vector<int> vids, aids;
	QStringList vnames, anames;
	for (const auto &s : snap) {
		const QString name = QString::fromStdString(s.name);
		if (s.kind == Kind::Video) {
			vids.push_back(s.id);
			vnames << name;
		} else {
			aids.push_back(s.id);
			anames << name;
		}
	}
	int v, a;
	Plugin::instance().resolvedPair(v, a);
	auto sync = [](QComboBox *combo, std::vector<int> &ids, const std::vector<int> &newIds, const QStringList &names,
		       int current) {
		bool same = ids == newIds && combo->count() == names.size();
		for (int i = 0; same && i < names.size(); i++)
			same = combo->itemText(i) == names[i];
		if (!same) {
			QSignalBlocker block(combo);
			combo->clear();
			combo->addItems(names);
			ids = newIds;
		}
		const auto it = std::find(ids.begin(), ids.end(), current);
		const int idx = it == ids.end() ? -1 : (int)(it - ids.begin());
		if (combo->currentIndex() != idx) {
			QSignalBlocker block(combo);
			combo->setCurrentIndex(idx);
		}
	};
	sync(videoCombo_, comboVideoIds_, vids, vnames, v);
	sync(audioCombo_, comboAudioIds_, aids, anames, a);
}

void AvSyncDock::refreshTable(const std::vector<ProbeSnapshot> &snap)
{
	if (table_->rowCount() != (int)snap.size()) {
		table_->setRowCount((int)snap.size());
		int h = table_->horizontalHeader()->sizeHint().height() + 2 * table_->frameWidth();
		for (int r = 0; r < table_->rowCount(); r++)
			h += table_->verticalHeader()->sectionSize(r);
		table_->setFixedHeight(std::max(h, 40));
	}
	const bool anyData = std::any_of(snap.begin(), snap.end(), [](const ProbeSnapshot &s) { return s.total.count > 0; });
	emptyHint_->setVisible(!anyData);

	static const QIcon green = dotIcon(kGreen), orange = dotIcon(kOrange), grey = dotIcon(kGrey);
	auto item = [this](int row, int col) {
		QTableWidgetItem *it = table_->item(row, col);
		if (!it) {
			it = new QTableWidgetItem();
			it->setTextAlignment(col == 0 ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignRight | Qt::AlignVCenter);
			table_->setItem(row, col, it);
		}
		return it;
	};
	for (int row = 0; row < (int)snap.size(); row++) {
		const ProbeSnapshot &s = snap[(size_t)row];
		const bool fresh = s.lastAgeMs >= 0 && s.lastAgeMs < 4000;
		QString name = (s.kind == Kind::Video ? QStringLiteral("🎥 ") : QStringLiteral("🎤 ")) + QString::fromStdString(s.name);
		QString status = QString::fromStdString(s.signal.text);
		if (s.live.count && !s.locked)
			status += QStringLiteral(" · ") + T("Dock.Status.Unverified");
		const QString tip = status + QStringLiteral("\n") +
				    T("Dock.Row.Tip")
					    .arg(ms(s.live.last), ms(s.live.mean), ms(s.live.min), ms(s.live.max), ms(s.total.median))
					    .arg(s.total.count);

		QTableWidgetItem *nameItem = item(row, 0);
		if (nameItem->text() != name)
			nameItem->setText(name);
		nameItem->setIcon(fresh ? green : s.signal.ok ? orange : grey);
		const QString values[3] = {s.live.count ? ms(s.live.median) : QStringLiteral("–"),
					   s.live.count > 1 ? ms(s.live.stddev) : QStringLiteral("–"),
					   QString::number(s.total.count)};
		for (int col = 0; col < 4; col++) {
			QTableWidgetItem *it = item(row, col);
			if (col > 0 && it->text() != values[col - 1])
				it->setText(values[col - 1]);
			if (it->toolTip() != tip)
				it->setToolTip(tip);
			const QColor color = fresh ? palette().color(QPalette::Text) : palette().color(QPalette::PlaceholderText);
			if (it->foreground().color() != color)
				it->setForeground(color);
		}
	}
}

void AvSyncDock::refreshOffset(const std::vector<ProbeSnapshot> &snap)
{
	Plugin &p = Plugin::instance();
	int v, a;
	p.resolvedPair(v, a);
	const ProbeSnapshot *vs = nullptr, *as = nullptr;
	for (const auto &s : snap) {
		if (s.id == v)
			vs = &s;
		if (s.id == a)
			as = &s;
	}
	const PairStats ps = p.engine().pair(v, a, (size_t)p.config().liveWindow);
	if (ps.offset.count == 0) {
		offsetLabel_->setText(QStringLiteral("Δ –"));
		offsetHint_->setText(vs && as ? T("Dock.Offset.Waiting") : T("Dock.Offset.NoPair"));
		offsetHint_->setToolTip(QString());
		applyButton_->setEnabled(false);
	} else {
		const double off = ps.offset.median;
		offsetLabel_->setText(QStringLiteral("Δ %1 ms").arg(signedMs(off)));
		QString hint, advice;
		if (std::fabs(off) < 5.0) {
			hint = T("Dock.Offset.InSync");
		} else if (off < 0) {
			hint = T("Dock.Offset.AudioEarly").arg(ms(-off, 0));
			advice = T("Dock.Offset.AudioEarly.Tip");
		} else {
			hint = T("Dock.Offset.AudioLate").arg(ms(off, 0));
			advice = T("Dock.Offset.AudioLate.Tip");
		}
		if (ps.relative)
			hint += QStringLiteral(" · ") + T("Dock.Offset.RelativeShort");
		QString tip = ps.relative ? T("Dock.Offset.Relative")
					  : T("Dock.Offset.Details")
						    .arg(ms(vs ? vs->live.median : NAN), ms(as ? as->live.median : NAN),
							 ms(ps.offset.stddev))
						    .arg(ps.offset.count);
		if (!advice.isEmpty())
			tip += QStringLiteral("\n") + advice;
		offsetHint_->setText(hint);
		offsetHint_->setToolTip(tip);
		offsetLabel_->setToolTip(tip);
		applyButton_->setEnabled(as && as->origin == "filter" && !as->sourceUuid.empty() && ps.offset.count >= 3 &&
					 std::fabs(off) >= 1.0);
	}

	std::vector<LatencyGraph::Series> series;
	if (vs) {
		LatencyGraph::Series s{kVideoColor, {}};
		for (const auto &m : vs->recent)
			s.points.push_back({m.seq, m.latencyMs});
		series.push_back(std::move(s));
	}
	if (as) {
		LatencyGraph::Series s{kAudioColor, {}};
		for (const auto &m : as->recent)
			s.points.push_back({m.seq, m.latencyMs});
		series.push_back(std::move(s));
	}
	if (ps.offset.count) {
		const PairStats longer = p.engine().pair(v, a, 60);
		LatencyGraph::Series s{kOffsetColor, {}};
		for (const auto &r : longer.recent)
			s.points.push_back({r.first, r.second});
		series.push_back(std::move(s));
	}
	graph_->setSeries(std::move(series));
}

void AvSyncDock::refreshSession()
{
	Plugin &p = Plugin::instance();
	const SessionResult r = p.engine().session();
	const double now = p.nowUtcMs();
	const QString startText = QDateTime::fromMSecsSinceEpoch((qint64)r.startUtcMs).toString(QStringLiteral("HH:mm:ss"));
	QString text, tip;
	switch (r.state) {
	case SessionState::Idle:
		break;
	case SessionState::Scheduled:
		text = T("Dock.Session.Scheduled").arg(startText, ms((r.startUtcMs - now) / 1000.0, 0));
		break;
	case SessionState::Running: {
		const double total = (r.endUtcMs - r.startUtcMs) / 1000.0;
		const double done = std::clamp((now - r.startUtcMs) / 1000.0, 0.0, total);
		text = T("Dock.Session.Running").arg(ms(done, 0), ms(total, 0));
		break;
	}
	case SessionState::Finished: {
		text = T("Dock.Session.Finished").arg(startText);
		if (r.pairOffset.count)
			text += QStringLiteral(" · Δ %1 ms").arg(signedMs(r.pairOffset.median));
		const std::string csv = p.lastCsvPath();
		const std::string err = p.lastCsvError();
		if (!csv.empty())
			text += QStringLiteral(" · <a href=\"%1\">CSV</a>").arg(QString::fromStdString(csv).toHtmlEscaped());
		if (!err.empty())
			text += QStringLiteral(" · <span style=\"color:#e05050\">%1</span>")
					.arg(QString::fromStdString(err).toHtmlEscaped());
		for (const auto &row : r.probes) {
			if (!row.stats.count)
				continue;
			tip += QStringLiteral("%1 %2: Median %3 ms, σ %4 ms (n=%5)\n")
				       .arg(row.kind == Kind::Video ? QStringLiteral("🎥") : QStringLiteral("🎤"),
					    QString::fromStdString(row.name), ms(row.stats.median), ms(row.stats.stddev))
				       .arg(row.stats.count);
		}
		if (!csv.empty())
			tip += QString::fromStdString(csv);
		break;
	}
	}
	if (sessionLabel_->text() != text)
		sessionLabel_->setText(text);
	sessionLabel_->setToolTip(tip.trimmed());
	sessionLabel_->setVisible(!text.isEmpty());
	cancelButton_->setEnabled(r.state == SessionState::Scheduled || r.state == SessionState::Running);
}

AvSyncDock *g_dock = nullptr;

} // namespace

void createDock()
{
	if (g_dock)
		return;
	g_dock = new AvSyncDock();
	g_dock->setObjectName(QStringLiteral("avsyncDock"));
	if (!obs_frontend_add_dock_by_id("avsync-dock", obs_module_text("Dock.Title"), g_dock)) {
		delete g_dock;
		g_dock = nullptr;
		return;
	}
	// test hook: AVSYNC_SHOW_DOCK=1 opens the dock after start-up
	if (getenv("AVSYNC_SHOW_DOCK")) {
		QTimer::singleShot(3000, [] {
			if (g_dock && g_dock->parentWidget()) {
				g_dock->parentWidget()->show();
				g_dock->parentWidget()->raise();
			}
		});
	}
}

void destroyDock()
{
	// The dock belongs to the OBS main window, which deletes it on shutdown.
	g_dock = nullptr;
}

} // namespace avs
