#pragma once
/*=========================================================================

 medInria

 Copyright (c) INRIA 2013 - 2020. All rights reserved.
 See LICENSE.txt for details.
 
  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

=========================================================================*/

#include <medAbstractPacsBaseScu.h>
#include <medPacsExport.h>

class medAbstractPacsMoveScu;

class MEDPACS_EXPORT medAbstractPacsMoveScu : public medAbstractPacsBaseScu
{
    Q_OBJECT

public:
    medAbstractPacsMoveScu() = default;
    ~medAbstractPacsMoveScu() override = default;

    virtual int sendMoveRequest(const char* peerTitle, const char* peerIP, unsigned int peerPort, 
                                const char* ourTitle, const char* ourIP, unsigned int ourPort);

    virtual void sendCancelRequest();

    virtual void useBuildInStoreSCP(bool flag);

    virtual bool setStorageDirectory(const char* directory);

    virtual bool addRequestToQueue(int group, int elem, const char* query, medAbstractPacsNode& moveSource, medAbstractPacsNode& moveTarget);

    virtual int performQueuedMoveRequests();

signals:
    void progressed(int);
};
