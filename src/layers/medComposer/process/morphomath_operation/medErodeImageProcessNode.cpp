/*=========================================================================

 medInria

 Copyright (c) INRIA 2013 - 2018. All rights reserved.
 See LICENSE.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

=========================================================================*/

#include <medErodeImageProcessNode.h>

#include <medCore.h>

medErodeImageProcessNode::medErodeImageProcessNode()
{
    this->setFactory(medCore::morphomathOperation::erodeImage::pluginFactory());
}


