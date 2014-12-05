/*=========================================================================

 medInria

 Copyright (c) INRIA 2013 - 2014. All rights reserved.
 See LICENSE.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

=========================================================================*/

#pragma once

#include <dtkComposer/dtkComposer.h>
#include "medComposerExport.h"

class medComposerPrivate;
class dtkComposerFactory;

class MEDCOMPOSER_EXPORT medComposer : public dtkComposer
{
    Q_OBJECT

public:
    medComposer(QWidget* parent = 0);

    void setFactory(dtkComposerFactory *factory);

public slots:
    void run(void);

private:
    medComposerPrivate *d;
};


