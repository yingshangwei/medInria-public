/* medLayoutChooser.h --- 
 * 
 * Author: Julien Wintz
 * Copyright (C) 2008 - Julien Wintz, Inria.
 * Created: Fri Oct 16 15:50:18 2009 (+0200)
 * Version: $Id$
 * Last-Updated: Fri Oct 16 15:50:21 2009 (+0200)
 *           By: Julien Wintz
 *     Update #: 1
 */

/* Commentary: 
 * 
 */

/* Change log:
 * 
 */

#ifndef MEDLAYOUTCHOOSER_H
#define MEDLAYOUTCHOOSER_H

#include <QTableWidget>

class medLayoutChooserPrivate;

class medLayoutChooser : public QTableWidget
{
    Q_OBJECT

public:
     medLayoutChooser(QWidget *parent = 0);
    ~medLayoutChooser(void);

    QSize sizeHint(void) const;

    int sizeHintForRow(int row) const;
    int sizeHintForColumn(int column) const;

signals:
    void selected(int rows, int cols);

protected:
    void mousePressEvent(QMouseEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);

protected slots:
    void onSelectionChanged(const QItemSelection& selected, const QItemSelection& unselected);

private:
    medLayoutChooserPrivate *d;
};

#endif // MEDLAYOUTCHOOSER_H