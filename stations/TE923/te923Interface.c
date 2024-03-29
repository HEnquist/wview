/*---------------------------------------------------------------------------
 
  FILENAME:
        te923Interface.c
 
  PURPOSE:
        Provide the Honeywell TE923 station interface API and utilities.
 
  REVISION HISTORY:
        Date            Engineer        Revision        Remarks
        02/17/2011      M.S. Teel       0               Original
 
  NOTES:
        The TE923 station provides a USB HID interface for I/O        
 
  LICENSE:
  
        This source code is released for free distribution under the terms 
        of the GNU General Public License.
  
----------------------------------------------------------------------------*/

/*  ... System include files
*/

/*  ... Library include files
*/

/*  ... Local include files
*/
#include <te923Interface.h>

/*  ... global memory declarations
*/

/*  ... local memory
*/

static TE923_IF_DATA   te923WorkData;
static void             (*ArchiveIndicator) (ARCHIVE_PKT* newRecord);

static void serialPortConfig (int fd);



////////////****////****  S T A T I O N   A P I  ****////****////////////
/////  Must be provided by each supported wview station interface  //////

// station-supplied init function
// -- Can Be Asynchronous - event indication required --
//
// MUST:
//   - set the 'stationGeneratesArchives' flag in WVIEWD_WORK:
//     if the station generates archive records (TRUE) or they should be
//     generated automatically by the daemon from the sensor readings (FALSE)
//   - Initialize the 'stationData' store for station work area
//   - Initialize the interface medium
//   - do initial LOOP acquisition
//   - do any catch-up on archive records if there is a data logger
//   - 'work->runningFlag' can be used for start up synchronization but should
//     not be modified by the station interface code
//   - indicate init is done by sending the STATION_INIT_COMPLETE_EVENT event to
//     this process (radProcessEventsSend (NULL, STATION_INIT_COMPLETE_EVENT, 0))
//
// OPTIONAL:
//   - Initialize a state machine or any other construct required for the
//     station interface - these should be stored in the 'stationData' store
//
// 'archiveIndication' - indication callback used to pass back an archive record
//   generated as a result of 'stationGetArchive' being called; should receive a
//   NULL pointer for 'newRecord' if no record available; only used if
//   'stationGeneratesArchives' flag is set to TRUE by the station interface
//
// Returns: OK or ERROR
//
int stationInit
(
    WVIEWD_WORK     *work,
    void            (*archiveIndication)(ARCHIVE_PKT* newRecord)
)
{
    int             i;
    STIM            stim;

    memset (&te923WorkData, 0, sizeof(te923WorkData));

    // save the archive indication callback (we should never need it)
    ArchiveIndicator = archiveIndication;

    // set our work data pointer
    work->stationData = &te923WorkData;

    // set the Archive Generation flag to indicate the WMR918 DOES NOT
    // generate them
    work->stationGeneratesArchives = FALSE;

    // The TE923 is a USB-only device:
    if (usbhidMediumInit (&work->medium, TE923_VENDOR_ID, TE923_PRODUCT_ID, FALSE) == ERROR)
    {
        radMsgLog (PRI_HIGH, "stationInit: USB MediumInit failed");
        return ERROR;
    }

    radMsgLog (PRI_STATUS, "TE923 on USB %4.4X:%4.4X opened ...",
               TE923_VENDOR_ID, TE923_PRODUCT_ID);


    // grab the station configuration now
    if (stationGetConfigValueInt (work,
                                  STATION_PARM_ELEVATION,
                                  &te923WorkData.elevation)
            == ERROR)
    {
        radMsgLog (PRI_HIGH, "stationInit: stationGetConfigValueInt ELEV failed!");
        (*(work->medium.usbhidExit)) (&work->medium);
        return ERROR;
    }
    if (stationGetConfigValueFloat (work,
                                    STATION_PARM_LATITUDE,
                                    &te923WorkData.latitude)
            == ERROR)
    {
        radMsgLog (PRI_HIGH, "stationInit: stationGetConfigValueInt LAT failed!");
        (*(work->medium.usbhidExit)) (&work->medium);
        return ERROR;
    }
    if (stationGetConfigValueFloat (work,
                                    STATION_PARM_LONGITUDE,
                                    &te923WorkData.longitude)
            == ERROR)
    {
        radMsgLog (PRI_HIGH, "stationInit: stationGetConfigValueInt LONG failed!");
        (*(work->medium.usbhidExit)) (&work->medium);
        return ERROR;
    }
    if (stationGetConfigValueInt (work,
                                  STATION_PARM_ARC_INTERVAL,
                                  &te923WorkData.archiveInterval)
            == ERROR)
    {
        radMsgLog (PRI_HIGH, "stationInit: stationGetConfigValueInt ARCINT failed!");
        (*(work->medium.usbhidExit)) (&work->medium);
        return ERROR;
    }

    // set the work archive interval now
    work->archiveInterval = te923WorkData.archiveInterval;

    // sanity check the archive interval against the most recent record
    if (stationVerifyArchiveInterval (work) == ERROR)
    {
        // bad magic!
        radMsgLog (PRI_HIGH, "stationInit: stationVerifyArchiveInterval failed!");
        radMsgLog (PRI_HIGH, "You must either move old archive data out of the way -or-");
        radMsgLog (PRI_HIGH, "fix the interval setting...");
        (*(work->medium.usbhidExit)) (&work->medium);
        return ERROR;
    }
    else
    {
        radMsgLog (PRI_STATUS, "station archive interval: %d minutes",
                   work->archiveInterval);
    }

    radMsgLog (PRI_STATUS, "Starting station interface: TE923"); 

    if (te923Init (work) == ERROR)
    {
        radMsgLog (PRI_HIGH, "stationInit: te923Init failed!");
        (*(work->medium.usbhidExit)) (&work->medium);
        return ERROR;
    }

    return OK;
}

// station-supplied exit function
//
// Returns: N/A
//
void stationExit (WVIEWD_WORK *work)
{
    te923Exit (work);
    (*(work->medium.usbhidExit)) (&work->medium);

    return;
}

// station-supplied function to retrieve positional info (lat, long, elev) -
// should populate 'work' fields: latitude, longitude, elevation
// -- Synchronous --
//
// - If station does not store these parameters, they can be retrieved from the
//   wview.conf file (see daemon.c for example conf file use) - user must choose
//   station type "Generic" when running the wviewconfig script
//
// Returns: OK or ERROR
//
int stationGetPosition (WVIEWD_WORK *work)
{
    // just set the values from our internal store - we retrieved them in
    // stationInit
    work->elevation     = (int16_t)te923WorkData.elevation;
    if (te923WorkData.latitude >= 0)
        work->latitude      = (int16_t)((te923WorkData.latitude*10)+0.5);
    else
        work->latitude      = (int16_t)((te923WorkData.latitude*10)-0.5);
    if (te923WorkData.longitude >= 0)
        work->longitude     = (int16_t)((te923WorkData.longitude*10)+0.5);
    else
        work->longitude     = (int16_t)((te923WorkData.longitude*10)-0.5);

    radMsgLog (PRI_STATUS, "station location: elevation: %d feet",
               work->elevation);

    radMsgLog (PRI_STATUS, "station location: latitude: %3.1f %c  longitude: %3.1f %c",
               (float)abs(work->latitude)/10.0,
               ((work->latitude < 0) ? 'S' : 'N'),
               (float)abs(work->longitude)/10.0,
               ((work->longitude < 0) ? 'W' : 'E'));

    return OK;
}

// station-supplied function to indicate a time sync should be performed if the
// station maintains time, otherwise may be safely ignored
// -- Can Be Asynchronous --
//
// Returns: OK or ERROR
//
int stationSyncTime (WVIEWD_WORK *work)
{
    // We don't use the TE923 time...
    return OK;
}

// station-supplied function to indicate sensor readings should be performed -
// should populate 'work' struct: loopPkt (see datadefs.h for minimum field reqs)
// -- Can Be Asynchronous --
//
// - indicate readings are complete by sending the STATION_LOOP_COMPLETE_EVENT
//   event to this process (radProcessEventsSend (NULL, STATION_LOOP_COMPLETE_EVENT, 0))
//
// Returns: OK or ERROR
//
int stationGetReadings (WVIEWD_WORK *work)
{
    te923GetReadings (work);

    return OK;
}

// station-supplied function to indicate an archive record should be generated -
// MUST populate an ARCHIVE_RECORD struct and indicate it to 'archiveIndication'
// function passed into 'stationInit'
// -- Asynchronous - callback indication required --
//
// Returns: OK or ERROR
//
// Note: 'archiveIndication' should receive a NULL pointer for the newRecord if
//       no record is available
// Note: This function will only be invoked by the wview daemon if the
//       'stationInit' function set the 'stationGeneratesArchives' to TRUE
//
int stationGetArchive (WVIEWD_WORK *work)
{
    // just indicate a NULL record, WMR918 does not generate them (and this
    // function should never be called!)
    (*ArchiveIndicator) (NULL);
    return OK;
}

// station-supplied function to indicate data is available on the station
// interface medium (serial or ethernet) -
// It is the responsibility of the station interface to read the data from the
// medium and process appropriately. The data does not have to be read within
// the context of this function, but may be used to stimulate a state machine.
// -- Synchronous --
//
// Returns: N/A
//
void stationDataIndicate (WVIEWD_WORK *work)
{
    // N/A
    return;
}

// station-supplied function to receive IPM messages - any message received by
// the generic station message handler which is not recognized will be passed
// to the station-specific code through this function.
// It is the responsibility of the station interface to process the message 
// appropriately (or ignore it).
// -- Synchronous --
//
// Returns: N/A
//
void stationMessageIndicate (WVIEWD_WORK *work, int msgType, void *msg)
{
    // N/A
    return;
}

// station-supplied function to indicate the interface timer has expired -
// It is the responsibility of the station interface to start/stop the interface
// timer as needed for the particular station requirements.
// The station interface timer is specified by the 'ifTimer' member of the
// WVIEWD_WORK structure. No other timers in that structure should be manipulated
// in any way by the station interface code.
// -- Synchronous --
//
// Returns: N/A
//
void stationIFTimerExpiry (WVIEWD_WORK *work)
{
    // N/A
    return;
}

////////////****////  S T A T I O N   A P I   E N D  ////****////////////

