/* medDatabasePreview.h --- 
 * 
 * Author: Julien Wintz
 * Copyright (C) 2008 - Julien Wintz, Inria.
 * Created: Tue Dec 15 09:42:06 2009 (+0100)
 * Version: $Id$
 * Last-Updated: Tue Dec 15 09:42:06 2009 (+0100)
 *           By: Julien Wintz
 *     Update #: 1
 */

/* Commentary: 
 * 
 */

/* Change log:
 * 
 */

#ifndef MEDDATABASEPREVIEW_H
#define MEDDATABASEPREVIEW_H

#include "medSqlExport.h"

#include <QtCore>
#include <QtGui>

class medDatabasePreviewItem;
class medDatabasePreviewPrivate;

class MEDSQL_EXPORT medDatabasePreview : public QFrame
{
    Q_OBJECT

public:
     medDatabasePreview(QWidget *parent = 0);
    ~medDatabasePreview(void);

    void init(void);
    void reset(void);

signals:
    void patientClicked(int id);
    void   studyClicked(int id);
    void  seriesClicked(int id);
    void   imageClicked(int id);

public slots:
    void onPatientClicked(int id);
    void   onStudyClicked(int id);
    void  onSeriesClicked(int id);
    void   onImageClicked(int id);

protected slots:
    void onSlideUp(void);
    void onSlideDw(void);

    void onMoveRt(void);
    void onMoveLt(void);
    void onMoveUp(void);
    void onMoveDw(void);
    void onMoveBg(void);

    void onHovered(medDatabasePreviewItem *item);

private:
    medDatabasePreviewPrivate *d;
};

#endif // MEDDATABASEPREVIEW_H