/*=========================================================================

 medInria

 Copyright (c) INRIA 2013 - 2014. All rights reserved.
 See LICENSE.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

=========================================================================*/

#pragma once

#include <medRpiApplyTransformationExport.h>
#include <medAbstractApplyTransformationProcess.h>


class medAbstractTransformation;

class medRpiApplyTransformationProcessPrivate;
class MEDRPIAPPLYTRANSFORMATIONPLUGIN_EXPORT medRpiApplyTransformationProcess : public medAbstractApplyTransformationProcess
{
    Q_OBJECT

public:

    medRpiApplyTransformationProcess();
    virtual ~medRpiApplyTransformationProcess();

public:
    virtual QString description() const;
    virtual QString identifier() const;
    static bool registered();

public:
    virtual bool isInteractive() const;

public:
    virtual int update();

public:
    void addTransformation(medAbstractTransformation *transfo);
    void addTransformation(QList<medAbstractTransformation *> transfo);
    QList<medAbstractTransformation *> transformationStack() const;

public:
    void resetTransformationStack();

public:
    void setGeometry(medAbstractImageData *geometry);
    medAbstractImageData*  geometry() const;
    void setInputImage(medAbstractImageData *imageData);
    medAbstractImageData* inputImage() const;

public:
    QList<medAbstractParameter*> parameters();

private:
    medRpiApplyTransformationProcessPrivate *d;
};


dtkAbstractProcess *createmedRpiApplyTransformation();


