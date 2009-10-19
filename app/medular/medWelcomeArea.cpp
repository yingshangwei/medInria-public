/* medWelcomeArea.cpp --- 
 * 
 * Author: Julien Wintz
 * Copyright (C) 2008 - Julien Wintz, Inria.
 * Created: Mon Oct  5 08:29:35 2009 (+0200)
 * Version: $Id$
 * Last-Updated: Thu Oct 15 16:08:10 2009 (+0200)
 *           By: Julien Wintz
 *     Update #: 141
 */

/* Commentary: 
 * 
 */

/* Change log:
 * 
 */

#include "medWelcomeArea.h"

#include <QtCore>
#include <QtGui>
#include <QtWebKit>

#include <medGui/medLoginWidget.h>

static QString readFile(const QString &name)
{
    QFile f(name);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("Unable to open %s: %s", name.toUtf8().constData(), f.errorString().toUtf8().constData());
        return QString();
    }
    QTextStream ts(&f);
    return ts.readAll();
}

class medWelcomeAreaPrivate
{
public:
    QWebView *web_view;

    medLoginWidget *loginWidget;
};

medWelcomeArea::medWelcomeArea(QWidget *parent) : QWidget(parent), d(new medWelcomeAreaPrivate)
{
    // d->web_view = new QWebView(this);
    // d->web_view->setContextMenuPolicy(Qt::NoContextMenu);
    // d->web_view->setAcceptDrops(false);
    // d->web_view->settings()->setAttribute(QWebSettings::PluginsEnabled, true);
    // d->web_view->settings()->setAttribute(QWebSettings::JavaEnabled, true);
    // // d->web_view->setHtml(readFile(QLatin1String(":/html/index.html")), QUrl("qrc:/html/index.html"));
    // d->web_view->setUrl(QUrl("http://www.youtube.com/html5"));

    // // qDebug() << d->web_view->settings()->pluginDatabase()->searchPaths();

    // connect(d->web_view, SIGNAL(linkClicked(QUrl)), this, SLOT(linkClicked(QUrl)));

    d->loginWidget = new medLoginWidget(this);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // layout->addWidget(d->web_view);
    layout->addStretch(8);
    layout->addWidget(d->loginWidget);
    layout->addStretch(1);
}

medWelcomeArea::~medWelcomeArea(void)
{
    delete d;

    d = NULL;
}

void medWelcomeArea::paintEvent(QPaintEvent *event)
{
    QFont font("Helvetica", 96);

    QFontMetrics metrics(font);
    int textWidth = metrics.width(qApp->applicationName());
    int textHeight = metrics.height();

    QRadialGradient gradient;
    gradient.setCenter(event->rect().center());
    gradient.setFocalPoint(event->rect().center());
    gradient.setRadius(event->rect().height()*0.66);
    gradient.setColorAt(0.0, QColor(0x49, 0x49, 0x49));
    // gradient.setColorAt(0.3, QColor(0x46, 0x46, 0x46));
    // gradient.setColorAt(0.5, QColor(0x40, 0x40, 0x40));
    // gradient.setColorAt(0.8, QColor(0x34, 0x34, 0x34));
    gradient.setColorAt(1.0, QColor(0x31, 0x31, 0x31));

    QPainter painter;
    painter.begin(this);
    painter.setRenderHints(QPainter::Antialiasing);
    painter.fillRect(event->rect(), gradient);
    painter.setPen(QColor(0x36, 0x36, 0x36));
    painter.setFont(font);
    painter.drawText(event->rect().center().x()-textWidth/2, event->rect().center().y(), qApp->applicationName());
    painter.end();

    QWidget::paintEvent(event);
}

void medWelcomeArea::linkClicked(const QUrl& url)
{
    Q_UNUSED(url);
}