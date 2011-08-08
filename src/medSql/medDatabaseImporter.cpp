/* medDatabaseImporter.cpp ---
 *
 * Author: Julien Wintz
 * Copyright (C) 2008 - Julien Wintz, Inria.
 * Created: Tue Jan 19 13:42:32 2010 (+0100)
 * Version: $Id$
 * Last-Updated: Thu Oct  7 15:48:22 2010 (+0200)
 *           By: Julien Wintz
 *     Update #: 48
 */

/* Commentary:
 *
 */

/* Change log:
 *
 */

#include "medDatabaseImporter.h"

#include <medAbstractDataImage.h>

#include <dtkCore/dtkAbstractDataFactory.h>
#include <dtkCore/dtkAbstractDataReader.h>
#include <dtkCore/dtkAbstractDataWriter.h>
#include <dtkCore/dtkAbstractData.h>
#include <dtkCore/dtkGlobal.h>
#include <dtkCore/dtkLog.h>
#include <medDatabaseController.h>
#include <medMetaDataHelper.h>
#include <medStorage.h>

class medDatabaseImporterPrivate
{
public:
    QString file;
    static QMutex mutex;
    QString lastSuccessfulReaderDescription;
    QString lastSuccessfulWriterDescription;
    bool isCancelled;
    bool indexWithoutImporting;

    // example of item in the list: ["patient", "study", "series"]
    QList<QStringList> partialAttemptsInfo;
};

QMutex medDatabaseImporterPrivate::mutex;

//-----------------------------------------------------------------------------------------------------------

medDatabaseImporter::medDatabaseImporter(const QString& file, bool indexWithoutImporting = false) : medJobItem(), d(new medDatabaseImporterPrivate)
{
    d->isCancelled = false;
    d->lastSuccessfulReaderDescription = "";
    d->lastSuccessfulWriterDescription = "";
    d->file = file;
    d->indexWithoutImporting = indexWithoutImporting;
}

//-----------------------------------------------------------------------------------------------------------

medDatabaseImporter::~medDatabaseImporter(void)
{
    delete d;

    d = NULL;
}

//-----------------------------------------------------------------------------------------------------------

void medDatabaseImporter::run(void)
{
    QMutexLocker locker(&d->mutex);

    /* The idea of this algorithm can be summarized in 3 steps:
     * 1. Get a list of all the files that will (try to) be imported or indexed
     * 2. Filter files that cannot be read, or won't be possible to write afterwards, or are already in the db
     * 3. Fill files metadata, write them to the db, and populate db tables
     *
     * note that depending on the input files, they might be aggregated by volume
     */

    // 1) Obtain a list of all the files that are going to be processed
    // this flattens the tree structure (if the input is a directory)
    // and puts all the files in one single list
    QStringList fileList = getAllFilesToBeProcessed(d->file);

    // Files that pass the filters named above are grouped
    // by volume in this map and will be written in the db after.
    // the key will be the name of the aggregated file with the volume
    QMap<QString, QStringList> imagesGroupedByVolume;

    int currentFileNumber = 0; // this variable will be used only for calculating progress

    // if importing, and depending on the input files, they might be aggregated
    // that is: files corresponding to the same volume will be written
    // in a single output meta file (e.g. .mha)
    // this map is used to store a unique id per volume and its volume number
    QMap<QString, int> volumeUniqueIdToVolumeNumber;
    int volumeNumber = 1;

    // 2) Select (by filtering) files to be imported
    //
    // In this first loop we read the headers of all the images to be imported
    // and check if we don't have any problem in reading the file, the header
    // or in selecting a proper format to store the new file afterwards
    // new files ARE NOT written in medinria database yet, but are stored in a map for writing in a posterior step
    foreach (QString file, fileList)
    {
        if (d->isCancelled) // check if user cancelled the process
            break;

        emit progressed(this,((qreal)currentFileNumber/(qreal)fileList.count())*50.0); //TODO: reading and filtering represents 50% of the importing process?

        currentFileNumber++;

        QFileInfo fileInfo(file);

        dtkSmartPointer<dtkAbstractData> dtkData;

        // 2.1) Try reading file information, just the header not the whole file

        bool readOnlyImageInformation = true;
        dtkData = tryReadImages(QStringList(fileInfo.filePath()), readOnlyImageInformation);

        if (!dtkData)
        {
            qWarning() << "Reader was unable to read: " << fileInfo.filePath();
            continue;
        }

        // 2.2) Fill missing metadata
        populateMissingMetadata(dtkData, fileInfo.baseName());

        // 2.3) Generate an unique id for each volume
        // all images of the same volume should share the same id
        QString volumeId = generateUniqueVolumeId(dtkData);

        // check whether the image belongs to a new volume
        if (!volumeUniqueIdToVolumeNumber.contains(volumeId))
        {
            volumeUniqueIdToVolumeNumber[volumeId] = volumeNumber;
            volumeNumber++;
        }

        // 2.3) a) Determine future file name and path based on patient/study/series/image
        // i.e.: where we will write the imported image
        QString imageFileName = determineFutureImageFileName(dtkData, volumeUniqueIdToVolumeNumber[volumeId]);

        // 2.3) b) Find the proper extension according to the type of the data
        // i.e.: in which format we will write the file in our database
        QString futureExtension  = determineFutureImageExtensionByDataType(dtkData);

        // we care whether we can write the image or not if we are importing
        if (!d->indexWithoutImporting && futureExtension.isEmpty())
        {
            emit showError(this, tr("Could not save file due to unhandled data type: ") + dtkData->description(), 5000);

            continue;
        }

        imageFileName = imageFileName + futureExtension;

        // 2.3) c) Add the image to a map for writing them all in medinria's database in a posterior step

        // First check if patient/study/series/image path already exists in the database
        if (!checkIfExists(dtkData, fileInfo.fileName()))
            imagesGroupedByVolume[imageFileName] << fileInfo.filePath();

    }

    // some checks to see if the user cancelled or something failed
    if (d->isCancelled)
    {
        emit showError(this, tr("User cancelled import process"), 5000);
        emit cancelled(this);
        return;
    }


    // 3) Re-read selected files and re-populate them with missing metadata
    //    then write them to medinria db and populate db tables

    QMap<QString, QStringList>::const_iterator it = imagesGroupedByVolume.begin();

    // 3.1) first check is after the filtering we have something to import
    // maybe we had problems with all the files, or they were already in the database
    if (it == imagesGroupedByVolume.end())
    {
        // TODO we know if it's either one or the other error, we can make this error better...
        emit showError(this, tr("No compatible image found or all of them had been already imported."), 5000);
        emit failure(this);
        return;
    }
    else
        qDebug() << "Image map contains " << imagesGroupedByVolume.size() << " files";

    int imagesCount = imagesGroupedByVolume.count(); // used only to calculate progress
    int currentImageIndex = 0; // used only to calculate progress

    // final loop: re-read, re-populate and write to db
    for (; it != imagesGroupedByVolume.end(); it++)
    {
        emit progressed(this,((qreal)currentImageIndex/(qreal)imagesCount)*50.0 + 50.0); // 50? I do not think that reading all the headers is half the job...

        currentImageIndex++;

        QString aggregatedFileName = it.key(); // note that this file might be aggregating more than one input files
        QStringList filesPaths = it.value(); // input files being aggregated, might be only one or many

        //qDebug() << currentImageIndex << ": " << aggregatedFileName << "with " << filesPaths.size() << " files";

         dtkSmartPointer<dtkAbstractData> imageDtkData;

        QFileInfo imagefileInfo(filesPaths[0]);

        // 3.2) Try to read the whole image, not just the header
        bool readOnlyImageInformation = false;
        imageDtkData = tryReadImages(filesPaths, readOnlyImageInformation);

        if (imageDtkData)
        {
            // 3.3) a) re-populate missing metadata
            // as files might be aggregated we use the aggregated file name as SeriesDescription (if not provided, of course)
            populateMissingMetadata(imageDtkData, imagefileInfo.baseName());

            // 3.3) b) now we are able to add some more metadata
            addAdditionalMetaData(imageDtkData, aggregatedFileName, filesPaths);
           }
        else
        {
            qWarning() << "Could not repopulate data!";
            emit showError(this, tr ("Could not read data: ") + filesPaths[0], 5000);
            continue;
        }

        // check for partial import attempts
        if (isPartialImportAttempt(imageDtkData))
            continue;

        if(!d->indexWithoutImporting)
        {
            // create location to store file
            QFileInfo fileInfo( medStorage::dataLocation() + aggregatedFileName );
            if ( !fileInfo.dir().exists() && !medStorage::mkpath(fileInfo.dir().path()) )
            {
                qDebug() << "Cannot create directory: " << fileInfo.dir().path();
                continue;
            }

            // now writing file
            bool writeSuccess = tryWriteImage(fileInfo.filePath(), imageDtkData);

            if (!writeSuccess){
                emit showError(this, tr ("Could not save data file: ") + filesPaths[0], 5000);
                continue;
            }
        }

        // and finally we populate the database
        QFileInfo aggregatedFileNameFileInfo( aggregatedFileName );
        QString pathToStoreThumbnails = aggregatedFileNameFileInfo.dir().path() + "/" + aggregatedFileNameFileInfo.completeBaseName() + "/";
        this->populateDatabaseAndGenerateThumbnails(imageDtkData, pathToStoreThumbnails);
    } // end of the final loop


    // if a partial import was attempted we tell the user what to do
    // to perform a correct import next time
    if(d->partialAttemptsInfo.size() > 0)
    {
        QString process = d->indexWithoutImporting ? "index" : "import";
        QString msg = "It seems you are trying to " + process + " some images that belong to a volume which is already in the database." + "\n";
        msg += "For a more accurate " + process + " please first delete the following series: " + "\n" + "\n";

        foreach(QStringList info, d->partialAttemptsInfo)
        {
            msg += "Series: " + info[2] + " (from patient: " + info[0] + " and study: " + info[1] + "\n";
        }

        emit partialImportAttempted(msg);
    }

    emit progressed(this,100);
    emit success(this);
    medDataIndex index;
    emit addedIndex(index);
}

//-----------------------------------------------------------------------------------------------------------

void medDatabaseImporter::onCancel( QObject* )
{
    d->isCancelled = true;
}

bool medDatabaseImporter::isPartialImportAttempt(dtkAbstractData* dtkData)
{
    // here we check is the series we try to import is already in the database

    QSqlDatabase db = *(medDatabaseController::instance()->database());
    QSqlQuery query(db);

    QString patientName = dtkData->metaDataValues(medMetaDataHelper::KEY_PatientName())[0].simplified();

    query.prepare("SELECT id FROM patient WHERE name = :name");
    query.bindValue(":name", patientName);

    if(!query.exec())
        qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

    if(query.first())
    {
        int patientId = query.value(0).toInt();

        query.clear();

        QString studyName = dtkData->metaDataValues(medMetaDataHelper::KEY_StudyDescription())[0].simplified();
        QString studyUid = dtkData->metaDataValues(medMetaDataHelper::KEY_StudyID())[0];

        query.prepare("SELECT id FROM study WHERE patient = :patientId AND name = :studyName AND uid = :studyUid");
        query.bindValue(":patientId", patientId);
        query.bindValue(":studyName", studyName);
        query.bindValue(":studyUid", studyUid);

        if(!query.exec())
            qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

        if(query.first())
        {
            int studyId = query.value(0).toInt();

            query.clear();

            QString seriesName = dtkData->metaDataValues(medMetaDataHelper::KEY_SeriesDescription())[0].simplified();
            QString seriesUid = dtkData->metaDataValues(medMetaDataHelper::KEY_SeriesID())[0];
            QString orientation = dtkData->metaDataValues(medMetaDataHelper::KEY_Orientation())[0];
            QString seriesNumber = dtkData->metaDataValues(medMetaDataHelper::KEY_SeriesNumber())[0];
            QString sequenceName = dtkData->metaDataValues(medMetaDataHelper::KEY_SequenceName())[0];
            QString sliceThickness = dtkData->metaDataValues(medMetaDataHelper::KEY_SliceThickness())[0];
            QString rows = dtkData->metaDataValues(medMetaDataHelper::KEY_Rows())[0];
            QString columns = dtkData->metaDataValues(medMetaDataHelper::KEY_Columns())[0];

            query.prepare("SELECT * FROM series WHERE study = :studyId AND name = :seriesName AND uid = :seriesUid AND orientation = :orientation AND seriesNumber = :seriesNumber AND sequenceName = :sequenceName AND sliceThickness = :sliceThickness AND rows = :rows AND columns = :columns");
            query.bindValue(":studyId", studyId);
            query.bindValue(":seriesName", seriesName);
            query.bindValue(":seriesUid", seriesUid);
            query.bindValue(":orientation", orientation);
            query.bindValue(":seriesNumber", seriesNumber);
            query.bindValue(":sequenceName", sequenceName);
            query.bindValue(":sliceThickness", sliceThickness);
            query.bindValue(":rows", rows);
            query.bindValue(":columns", columns);

            if(!query.exec())
                qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

            if(query.first())
            {
                QStringList filePaths = dtkData->metaDataValues (medMetaDataHelper::KEY_FilePaths());
                d->partialAttemptsInfo << (QStringList() << patientName << studyName << seriesName << filePaths[0]);
                return true;
            }
        }
    }

    return false;
}

//-----------------------------------------------------------------------------------------------------------

void medDatabaseImporter::populateMissingMetadata( dtkAbstractData* dtkData, const QString seriesDescription )
{
    if (!dtkData)
    {
        qWarning() << "data invalid";
        return;
    }

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_PatientName()))
        dtkData->addMetaData(medMetaDataHelper::KEY_PatientName(), QStringList() << "John Doe");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_StudyDescription()))
        dtkData->addMetaData(medMetaDataHelper::KEY_StudyDescription(), QStringList() << "EmptyStudy");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_SeriesDescription()))
        dtkData->addMetaData(medMetaDataHelper::KEY_SeriesDescription(), QStringList() << seriesDescription);

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_StudyID()))
        dtkData->addMetaData(medMetaDataHelper::KEY_StudyID(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_SeriesID()))
        dtkData->addMetaData(medMetaDataHelper::KEY_SeriesID(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Orientation()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Orientation(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_SeriesNumber()))
        dtkData->addMetaData(medMetaDataHelper::KEY_SeriesNumber(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_SequenceName()))
        dtkData->addMetaData(medMetaDataHelper::KEY_SequenceName(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_SliceThickness()))
        dtkData->addMetaData(medMetaDataHelper::KEY_SliceThickness(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Rows()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Rows(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Columns()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Columns(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Age()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Age(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_BirthDate()))
        dtkData->addMetaData(medMetaDataHelper::KEY_BirthDate(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Gender()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Gender(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Description()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Description(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Modality()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Modality(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Protocol()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Protocol(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Comments()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Comments(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Status()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Status(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_AcquisitionDate()))
        dtkData->addMetaData(medMetaDataHelper::KEY_AcquisitionDate(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_ImportationDate()))
        dtkData->addMetaData(medMetaDataHelper::KEY_ImportationDate(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Referee()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Referee(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Performer()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Performer(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Institution()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Institution(), QStringList() << "");

    if(!dtkData->hasMetaData(medMetaDataHelper::KEY_Report()))
        dtkData->addMetaData(medMetaDataHelper::KEY_Report(), QStringList() << "");
}

//-----------------------------------------------------------------------------------------------------------

bool medDatabaseImporter::checkIfExists(dtkAbstractData* dtkdata, QString imageName)
{
    bool imageExists = false;

    QSqlQuery query(*(medDatabaseController::instance()->database()));

    // first we query patient table
    QString patientName = dtkdata->metaDataValues(medMetaDataHelper::KEY_PatientName())[0];

    query.prepare("SELECT id FROM patient WHERE name = :name");
    query.bindValue(":name", patientName);

    if(!query.exec())
        qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

    if(query.first())
    {
        QVariant patientId = query.value(0);
        // if patient already exists we now verify the study

        QString studyName = dtkdata->metaDataValues(medMetaDataHelper::KEY_StudyDescription())[0];
        QString studyUid  = dtkdata->metaDataValues(medMetaDataHelper::KEY_StudyID())[0];

        query.prepare("SELECT id FROM study WHERE patient = :patientId AND name = :name AND uid = :studyID");
        query.bindValue(":patientId", patientId);
        query.bindValue(":name", studyName);
        query.bindValue(":studyUid", studyUid);

        if(!query.exec())
            qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

        if(query.first())
        {
            QVariant studyId = query.value(0);
            // both patient and study exists, let's check series
            QString seriesName  = dtkdata->metaDataValues(medMetaDataHelper::KEY_SeriesDescription())[0];
            QString seriesUid = dtkdata->metaDataValues(medMetaDataHelper::KEY_SeriesID())[0];
            QString orientation = dtkdata->metaDataValues(medMetaDataHelper::KEY_Orientation())[0]; // orientation sometimes differ by a few digits, thus this is not reliable
            QString seriesNumber = dtkdata->metaDataValues(medMetaDataHelper::KEY_SeriesNumber())[0];
            QString sequenceName = dtkdata->metaDataValues(medMetaDataHelper::KEY_SequenceName())[0];
            QString sliceThickness = dtkdata->metaDataValues(medMetaDataHelper::KEY_SliceThickness())[0];
            QString rows = dtkdata->metaDataValues(medMetaDataHelper::KEY_Rows())[0];
            QString columns = dtkdata->metaDataValues(medMetaDataHelper::KEY_Columns())[0];

            query.prepare("SELECT id FROM series WHERE study = :studyId AND name = :name AND uid = :seriesUid AND orientation = :orientation AND seriesNumber = :seriesNumber AND sequenceName = :sequenceName AND sliceThickness = :sliceThickness AND rows = :rows AND columns = :columns");
            query.bindValue(":studyId", studyId);
            query.bindValue(":name", seriesName);
            query.bindValue(":seriesUid", seriesUid);
            query.bindValue(":orientation", orientation);
            query.bindValue(":seriesNumber", seriesNumber);
            query.bindValue(":sequenceName", sequenceName);
            query.bindValue(":sliceThickness", sliceThickness);
            query.bindValue(":rows", rows);
            query.bindValue(":columns", columns);

            if(!query.exec())
                qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

            if(query.first())
            {
                QVariant seriesId = query.value(0);

                // finally we check the image table

                query.prepare("SELECT id FROM image WHERE series = :seriesId AND name = :name");
                query.bindValue(":seriesId", seriesId);
                query.bindValue(":name", imageName);

                if(!query.exec())
                    qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

                if(query.first())
                {
                    imageExists = true;
                }
            }
        }
    }
    return imageExists;
}

//-----------------------------------------------------------------------------------------------------------

void medDatabaseImporter::populateDatabaseAndGenerateThumbnails(dtkAbstractData* dtkData, QString pathToStoreThumbnails)
{
    QSqlDatabase db = *(medDatabaseController::instance()->database());

    QStringList thumbPaths = generateThumbnails(dtkData, pathToStoreThumbnails);

    int patientId = getOrCreatePatient(dtkData, db);

    int studyId = getOrCreateStudy(dtkData, db, patientId);

    int seriesId = getOrCreateSeries(dtkData, db, studyId);

    createMissingImages(dtkData, db, seriesId, thumbPaths);
}

//-----------------------------------------------------------------------------------------------------------

QStringList medDatabaseImporter::generateThumbnails(dtkAbstractData* dtkData, QString pathToStoreThumbnails)
{
    QList<QImage> &thumbnails = dtkData->thumbnails();

    QStringList thumbPaths;

    if (!medStorage::mkpath(medStorage::dataLocation() + pathToStoreThumbnails))
        qDebug() << "Cannot create directory: " << pathToStoreThumbnails;

    for (int i=0; i < thumbnails.count(); i++)
    {
        QString thumb_name = pathToStoreThumbnails + QString().setNum(i) + ".png";
        thumbnails[i].save(medStorage::dataLocation() + thumb_name, "PNG");
        thumbPaths << thumb_name;
    }

    QImage refThumbnail = dtkData->thumbnail(); // representative thumbnail for PATIENT/STUDY/SERIES
    QString refThumbPath = pathToStoreThumbnails + "ref.png";
    refThumbnail.save (medStorage::dataLocation() + refThumbPath, "PNG");

    dtkData->addMetaData(medMetaDataHelper::KEY_ThumbnailPath(), refThumbPath);

    return thumbPaths;
}

//-----------------------------------------------------------------------------------------------------------

int medDatabaseImporter::getOrCreatePatient(dtkAbstractData* dtkData, QSqlDatabase db)
{
    int patientId = -1;

    QSqlQuery query(db);

    QString patientName = dtkData->metaDataValues(medMetaDataHelper::KEY_PatientName())[0].simplified();
    query.prepare("SELECT id FROM patient WHERE name = :name");
    query.bindValue(":name", patientName);

    if(!query.exec())
        qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

    if(query.first())
    {
        patientId = query.value(0).toInt();
    }
    else
    {
        QString refThumbPath = dtkData->metaDataValues(medMetaDataHelper::KEY_ThumbnailPath())[0];
        QString birthdate      = dtkData->metaDataValues(medMetaDataHelper::KEY_BirthDate())[0];
        QString gender         = dtkData->metaDataValues(medMetaDataHelper::KEY_Gender())[0];

        query.prepare("INSERT INTO patient (name, thumbnail, birthdate, gender) VALUES (:name, :thumbnail, :birthdate, :gender)");
        query.bindValue(":name", patientName);
        query.bindValue(":thumbnail", refThumbPath );
        query.bindValue(":birthdate", birthdate );
        query.bindValue(":gender",    gender );
        query.exec();

        patientId = query.lastInsertId().toInt();
    }

    return patientId;
}

int medDatabaseImporter::getOrCreateStudy(dtkAbstractData* dtkData, QSqlDatabase db, int patientId)
{
    int studyId = -1;

    QSqlQuery query(db);

    QString studyName   = dtkData->metaDataValues(medMetaDataHelper::KEY_StudyDescription())[0].simplified();
    QString studyUid    = dtkData->metaDataValues(medMetaDataHelper::KEY_StudyID())[0];

    query.prepare("SELECT id FROM study WHERE patient = :patientId AND name = :studyName AND uid = :studyUid");
    query.bindValue(":patientId", patientId);
    query.bindValue(":studyName", studyName);
    query.bindValue(":studyUid", studyUid);

    if(!query.exec())
        qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

    if(query.first())
    {
        studyId = query.value(0).toInt();
    }
    else
    {
        QString refThumbPath = dtkData->metaDataValues(medMetaDataHelper::KEY_ThumbnailPath())[0];

        query.prepare("INSERT INTO study (patient, name, uid, thumbnail) VALUES (:patientId, :studyName, :studyUid, :thumbnail)");
        query.bindValue(":patientId", patientId);
        query.bindValue(":studyName", studyName);
        query.bindValue(":studyUid", studyUid);
        query.bindValue(":thumbnail", refThumbPath );

        query.exec();

        studyId = query.lastInsertId().toInt();
    }

    return studyId;
}

int medDatabaseImporter::getOrCreateSeries(dtkAbstractData* dtkData, QSqlDatabase db, int studyId)
{
    int seriesId = -1;

    QSqlQuery query(db);

    QString seriesName     = dtkData->metaDataValues(medMetaDataHelper::KEY_SeriesDescription())[0].simplified();
    QString seriesUid     = dtkData->metaDataValues(medMetaDataHelper::KEY_SeriesID())[0];
    QString orientation    = dtkData->metaDataValues(medMetaDataHelper::KEY_Orientation())[0];
    QString seriesNumber   = dtkData->metaDataValues(medMetaDataHelper::KEY_SeriesNumber())[0];
    QString sequenceName   = dtkData->metaDataValues(medMetaDataHelper::KEY_SequenceName())[0];
    QString sliceThickness = dtkData->metaDataValues(medMetaDataHelper::KEY_SliceThickness())[0];
    QString rows           = dtkData->metaDataValues(medMetaDataHelper::KEY_Rows())[0];
    QString columns        = dtkData->metaDataValues(medMetaDataHelper::KEY_Columns())[0];

    query.prepare("SELECT * FROM series WHERE study = :studyId AND name = :seriesName AND uid = :seriesUid AND orientation = :orientation AND seriesNumber = :seriesNumber AND sequenceName = :sequenceName AND sliceThickness = :sliceThickness AND rows = :rows AND columns = :columns");
    query.bindValue(":studyId", studyId);
    query.bindValue(":seriesName", seriesName);
    query.bindValue(":seriesUid", seriesUid);
    query.bindValue(":orientation", orientation);
    query.bindValue(":seriesNumber", seriesNumber);
    query.bindValue(":sequenceName", sequenceName);
    query.bindValue(":sliceThickness", sliceThickness);
    query.bindValue(":rows", rows);
    query.bindValue(":columns", columns);

    if(!query.exec())
        qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

    if(query.first())
    {
        seriesId = query.value(0).toInt();
    }
    else
    {
        // if we are creating a new series while indexing then we need to empty
        // the column 'path', as there won't be a file aggregating the images

        QString seriesPath = "";
        if(!d->indexWithoutImporting)
            //seriesPath = dtkData->metaDataValues (medMetaDataHelper::KEY_FilePaths())[0];
            seriesPath = dtkData->metaDataValues (tr("FileName"))[0];
        int size               = dtkData->metaDataValues(medMetaDataHelper::KEY_Size())[0].toInt();
        QString refThumbPath   = dtkData->metaDataValues(medMetaDataHelper::KEY_ThumbnailPath())[0];
        QString age            = dtkData->metaDataValues(medMetaDataHelper::KEY_Age())[0];
        QString description    = dtkData->metaDataValues(medMetaDataHelper::KEY_Description())[0];
        QString modality       = dtkData->metaDataValues(medMetaDataHelper::KEY_Modality())[0];
        QString protocol       = dtkData->metaDataValues(medMetaDataHelper::KEY_Protocol())[0];
        QString comments       = dtkData->metaDataValues(medMetaDataHelper::KEY_Comments())[0];
        QString status         = dtkData->metaDataValues(medMetaDataHelper::KEY_Status())[0];
        QString acqdate        = dtkData->metaDataValues(medMetaDataHelper::KEY_AcquisitionDate())[0];
        QString importdate     = dtkData->metaDataValues(medMetaDataHelper::KEY_ImportationDate())[0];
        QString referee        = dtkData->metaDataValues(medMetaDataHelper::KEY_Referee())[0];
        QString performer      = dtkData->metaDataValues(medMetaDataHelper::KEY_Performer())[0];
        QString institution    = dtkData->metaDataValues(medMetaDataHelper::KEY_Institution())[0];
        QString report         = dtkData->metaDataValues(medMetaDataHelper::KEY_Report())[0];

        query.prepare("INSERT INTO series (study, size, name, path, uid, orientation, seriesNumber, sequenceName, sliceThickness, rows, columns, thumbnail, age, description, modality, protocol, comments, status, acquisitiondate, importationdate, referee, performer, institution, report) VALUES (:study, :size, :seriesName, :seriesPath, :seriesUid, :orientation, :seriesNumber, :sequenceName, :sliceThickness, :rows, :columns, :refThumbPath, :age, :description, :modality, :protocol, :comments, :status, :acquisitiondate, :importationdate, :referee, :performer, :institution, :report)");
        query.bindValue(":study",          studyId);
        query.bindValue(":size",           size);
        query.bindValue(":seriesName",           seriesName);
        query.bindValue(":seriesPath",           seriesPath);
        query.bindValue(":seriesUid",       seriesUid);
        query.bindValue(":orientation",    orientation);
        query.bindValue(":seriesNumber",   seriesNumber);
        query.bindValue(":sequenceName",   sequenceName);
        query.bindValue(":sliceThickness", sliceThickness);
        query.bindValue(":rows",           rows);
        query.bindValue(":columns",        columns);
        query.bindValue(":thumbnail",      refThumbPath );
        query.bindValue(":age",         age);
        query.bindValue(":description", description);
        query.bindValue(":modality",    modality);
        query.bindValue(":protocol",    protocol);
        query.bindValue(":comments",    comments);
        query.bindValue(":status",      status);
        query.bindValue(":acquisitiondate",     acqdate);
        query.bindValue(":importationdate",  importdate);
        query.bindValue(":referee",     referee);
        query.bindValue(":performer",   performer);
        query.bindValue(":institution", institution);
        query.bindValue(":report",      report);

        if(!query.exec())
            qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

        seriesId = query.lastInsertId().toInt();
    }

    return seriesId;
}

void medDatabaseImporter::createMissingImages(dtkAbstractData* dtkData, QSqlDatabase db, int seriesId, QStringList thumbPaths)
{
    QSqlQuery query(db);

    QStringList filePaths  = dtkData->metaDataValues(medMetaDataHelper::KEY_FilePaths());

    if (filePaths.count() == 1 && thumbPaths.count() > 1) // special case to 1 image and multiple thumbnails
    {
        QFileInfo fileInfo(filePaths[0]);
        for (int i = 0; i < thumbPaths.count(); i++)
        {
            query.prepare("SELECT id FROM image WHERE series = :seriesId AND name = :name");
            query.bindValue(":seriesId", seriesId);
            query.bindValue(":name", fileInfo.fileName() + QString().setNum(i));

            if (!query.exec())
                qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

            if (query.first())
            {
                ; //qDebug() << "Image" << file << "already in database";
            }
            else
            {
                query.prepare("INSERT INTO image (series, name, path, instance_path, thumbnail, isIndexed) VALUES (:series, :name, :path, :instance_path, :thumbnail, :isIndexed)");
                query.bindValue(":series", seriesId);
                query.bindValue(":name", fileInfo.fileName() + QString().setNum(i));
                query.bindValue(":path", fileInfo.filePath());
                query.bindValue(":thumbnail", thumbPaths[i]);
                query.bindValue(":isIndexed", d->indexWithoutImporting);

                // if we are indexing we want to leave the 'instance_path' column blank
                // as we will use the full path in 'path' column to load them
                QString relativeFilePath = dtkData->metaDataValues(tr("FileName"))[0];
                query.bindValue(":instance_path", d->indexWithoutImporting ? "" : relativeFilePath);

                if (!query.exec())
                    qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;
            }
        }
    }
    else
    {
        for (int i = 0; i < filePaths.count(); i++)
        {
            QFileInfo fileInfo(filePaths[i]);

            query.prepare("SELECT id FROM image WHERE series = :seriesId AND name = :name");
            query.bindValue(":seriesId", seriesId);
            query.bindValue(":name", fileInfo.fileName());

            if (!query.exec())
                qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;

            if (query.first())
            {
                ; //qDebug() << "Image" << file << "already in database";
            }
            else
            {
                query.prepare("INSERT INTO image (series, name, path, instance_path, thumbnail, isIndexed) VALUES (:series, :name, :path, :instance_path, :thumbnail, :isIndexed)");
                query.bindValue(":series", seriesId);
                query.bindValue(":name", fileInfo.fileName());
                query.bindValue(":path", fileInfo.filePath());
                query.bindValue(":isIndexed", d->indexWithoutImporting);

                // if we are indexing we want to leave the 'instance_path' column blank
                // as we will use the full path in 'path' column to load them
                QString relativeFilePath = dtkData->metaDataValues(tr("FileName"))[0];
                query.bindValue(":instance_path", d->indexWithoutImporting ? "" : relativeFilePath);

                if (i < thumbPaths.count())
                    query.bindValue(":thumbnail", thumbPaths[i]);
                else
                    query.bindValue(":thumbnail", "");

                if (!query.exec())
                    qDebug() << DTK_COLOR_FG_RED << query.lastError() << DTK_NO_COLOR;
            }
        }
    }
}

//-----------------------------------------------------------------------------------------------------------

dtkSmartPointer<dtkAbstractDataReader> medDatabaseImporter::getSuitableReader( QStringList filename )
{
    QList<QString> readers = dtkAbstractDataFactory::instance()->readers();

    // cycle through readers to see if the last used reader can handle the file
    dtkSmartPointer<dtkAbstractDataReader> dataReader;
    for (int i=0; i<readers.size(); i++) {
        dataReader = dtkAbstractDataFactory::instance()->readerSmartPointer(readers[i]);
        if (d->lastSuccessfulReaderDescription == dataReader->description() && dataReader->canRead( filename ))
        {
            dataReader->enableDeferredDeletion(false);
            return dataReader;
        }
    }

    for (int i=0; i<readers.size(); i++) {
        dataReader = dtkAbstractDataFactory::instance()->readerSmartPointer(readers[i]);
        if (dataReader->canRead( filename )){
            d->lastSuccessfulReaderDescription = dataReader->description();
            {
                dataReader->enableDeferredDeletion(false);
                return dataReader;
            }
        }
    }

    qWarning() << "No suitable reader found!";
    return NULL;
}

//-----------------------------------------------------------------------------------------------------------

dtkSmartPointer<dtkAbstractDataWriter> medDatabaseImporter::getSuitableWriter( QString filename, dtkAbstractData* dtkData )
{
    if (!dtkData)
        return NULL;

    QList<QString> writers = dtkAbstractDataFactory::instance()->writers();
    dtkSmartPointer<dtkAbstractDataWriter> dataWriter;
    // first try with the last
    for (int i=0; i<writers.size(); i++) {
        dataWriter = dtkAbstractDataFactory::instance()->writerSmartPointer(writers[i]);
        if (d->lastSuccessfulReaderDescription == dataWriter->description()) {

            if ( dataWriter->handled().contains(dtkData->description()) &&
                 dataWriter->canWrite( filename ) ) {

                d->lastSuccessfulWriterDescription = dataWriter->description();
                dataWriter->enableDeferredDeletion(false);
                return dataWriter;
            }
        }
    }

    // cycle all
    for (int i=0; i<writers.size(); i++) {
        dataWriter = dtkAbstractDataFactory::instance()->writerSmartPointer(writers[i]);

        if ( dataWriter->handled().contains(dtkData->description()) &&
             dataWriter->canWrite( filename ) ) {

            d->lastSuccessfulWriterDescription = dataWriter->description();
            dataWriter->enableDeferredDeletion(false);
            return dataWriter;
        }
    }
    return NULL;
}

//-----------------------------------------------------------------------------------------------------------

QStringList medDatabaseImporter::getAllFilesToBeProcessed(QString fileOrDirectory)
{
    QString file = fileOrDirectory;

    QDir dir(file);
    dir.setFilter(QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot);

    QStringList fileList;
    if (dir.exists())
    {
       QDirIterator directory_walker(file, QDir::Files, QDirIterator::Subdirectories);
       while (directory_walker.hasNext())
       {
           fileList << directory_walker.next();
       }
    }
    else
       fileList << file;

    fileList.sort();

    return fileList;
}

dtkSmartPointer<dtkAbstractData> medDatabaseImporter::tryReadImages(QStringList filesPaths, bool readOnlyImageInformation)
{
    dtkSmartPointer<dtkAbstractData> dtkData = 0;

    dtkSmartPointer<dtkAbstractDataReader> dataReader;
    dataReader = getSuitableReader(filesPaths);

    if (dataReader)
    {
        if (readOnlyImageInformation)
        {
            dataReader->readInformation( filesPaths );
        }
        else
        {
            dataReader->read( filesPaths );
        }

        dtkData = dataReader->data();
    }
    else
    {
        // we take the first one for debugging just for simplicity
        qWarning() << "No suitable reader found for file: " << filesPaths[0] << ". Unable to import!";
    }

    return dtkData;
}

QString medDatabaseImporter::determineFutureImageFileName(const dtkAbstractData* dtkdata, int volumeNumber)
{
    // we append the uniqueID at the end of the filename to have unique filenames for each volume
    QString s_volumeNumber;
    s_volumeNumber.setNum(volumeNumber);

    QString patientName = dtkdata->metaDataValues(medMetaDataHelper::KEY_PatientName())[0];
    QString studyName   = dtkdata->metaDataValues(medMetaDataHelper::KEY_StudyDescription())[0];
    QString seriesName  = dtkdata->metaDataValues(medMetaDataHelper::KEY_SeriesDescription())[0];

    QString s_patientName = patientName.simplified();
    QString s_studyName   = studyName.simplified();
    QString s_seriesName  = seriesName.simplified();

    s_patientName.replace (0x00EA, 'e');
    s_studyName.replace   (0x00EA, 'e');
    s_seriesName.replace  (0x00EA, 'e');
    s_patientName.replace (0x00E4, 'a');
    s_studyName.replace   (0x00E4, 'a');
    s_seriesName.replace  (0x00E4, 'a');

    QString imageFileName = "/" + s_patientName + "/" +
            s_studyName   + "/" +
            s_seriesName  + s_volumeNumber;

    return imageFileName;
}

QString medDatabaseImporter::determineFutureImageExtensionByDataType(const dtkAbstractData* dtkdata)
{
    QString description = dtkdata->description();
    QString extension = "";

     // Determine the appropriate extension to use according to the type of data.
     // TODO: The image type is weakly recognized (contains("Image")). to be improved
     if (description == "vtkDataMesh")
     {
         extension = ".vtk";
         qDebug() << "vtkDataMesh";
     }
     else if (description == "vtkDataMesh4D")
     {
         extension = ".v4d";
         qDebug() << "vtkDataMesh4D";
     }
     else if (description == "v3dDataFibers")
     {
         extension = ".xml";
         qDebug() << "vtkDataMesh4D";
     }
     else if (description.contains("vistal"))
     {
         extension = ".dim";
         qDebug() << "Vistal Image";
     }
     else if (description.contains ("Image"))
     {
         extension = ".mha";
         //qDebug() << description;
     }

     return extension;
}

bool medDatabaseImporter::tryWriteImage(QString filePath, dtkAbstractData* imData)
{
    bool writeSuccess = false;

    dtkSmartPointer<dtkAbstractDataWriter> dataWriter = getSuitableWriter(filePath, imData);
    if( dataWriter)
    {
        dataWriter->setData (imData);
        if ( dataWriter->write(filePath))
        {
            writeSuccess = true;
        }
    }
    return writeSuccess;
}

void medDatabaseImporter::addAdditionalMetaData(dtkAbstractData* imData, QString aggregatedFileName, QStringList aggregatedFilesPaths)
{
    QStringList size;
    if (medAbstractDataImage *imageData = dynamic_cast<medAbstractDataImage*>(imData))
    {
        size << QString::number( imageData->zDimension() );
    }
    else {
        size << "";
    }

    imData->setMetaData ("Size", size);

    if (!imData->hasMetaData ("FilePaths"))
        imData->addMetaData  ("FilePaths", aggregatedFilesPaths);

    imData->addMetaData ("FileName", aggregatedFileName);
}

QString medDatabaseImporter::generateUniqueVolumeId(const dtkAbstractData* dtkData)
{
    if (!dtkData)
    {
        qWarning() << "data invalid";
        return "invalid";
    }

    // Get all the information from the dtkAbstractData metadata.
    // This information will then be passed to the database.
    QString patientName = dtkData->metaDataValues(medMetaDataHelper::KEY_PatientName())[0];
//    QString studyName   = dtkData->metaDataValues(medMetaDataHelper::KEY_StudyDescription())[0];
//    QString seriesName  = dtkData->metaDataValues(medMetaDataHelper::KEY_SeriesDescription())[0];

    QString studyId = dtkData->metaDataValues(medMetaDataHelper::KEY_StudyID())[0];
    QString seriesId = dtkData->metaDataValues(medMetaDataHelper::KEY_SeriesID())[0];
    QString orientation = dtkData->metaDataValues(medMetaDataHelper::KEY_Orientation())[0]; // orientation sometimes differ by a few digits, thus this is not reliable
    QString seriesNumber = dtkData->metaDataValues(medMetaDataHelper::KEY_SeriesNumber())[0];
    QString sequenceName = dtkData->metaDataValues(medMetaDataHelper::KEY_SequenceName())[0];
    QString sliceThickness = dtkData->metaDataValues(medMetaDataHelper::KEY_SliceThickness())[0];
    QString rows = dtkData->metaDataValues(medMetaDataHelper::KEY_Rows())[0];
    QString columns = dtkData->metaDataValues(medMetaDataHelper::KEY_Columns())[0];

    QStringList orientations = orientation.split(" ");

    orientation = "";

    // truncate orientation to 5 digits for a more robust import since
    // sometimes orientation only differs with the last 2 digits, creating
    // multiple series
    foreach(QString orient, orientations)
    {
        double d_orient = orient.toDouble();
        orientation += QString::number(d_orient, 'g', 5);
    }

    // define a unique key string to identify which volume an image belongs to.
    // we use: patientName, studyId, seriesId, orientation, seriesNumber, sequenceName, sliceThickness, rows, columns.
    // All images of the same volume should share similar values of these parameters
    QString key = patientName+studyId+seriesId+orientation+seriesNumber+sequenceName+sliceThickness+rows+columns;

    return key;
}
