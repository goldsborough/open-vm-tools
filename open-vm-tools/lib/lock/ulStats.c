/*********************************************************
 * Copyright (C) 2010 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

#include <math.h>  // for sqrt; should be removed soon

#if defined(_WIN32)
#include <intrin.h>
#endif

#include "vmware.h"
#include "str.h"
#include "util.h"
#include "userlock.h"
#include "ulInt.h"
#include "hostinfo.h"
#include "log.h"
#include "logFixed.h"
#if defined(MXUSER_STATS)
#include "statsLog.h"
#endif

#define BINS_PER_DECADE 100

static double mxUserContentionRatio = 0.0;  // always "off"
static uint64 mxUserContentionCount = 0;    // always "off"

#if defined(MXUSER_STATS)
static Atomic_Ptr mxLockMemPtr;   // internal singleton lock
static ListItem *mxUserLockList;  // list of all MXUser locks
#endif

struct MXUserHisto {
   char    *typeName;      // Type (name) of histogram
   uint64  *binData;       // hash table bins
   uint64   totalSamples;  // Population sample size
   uint64   minValue;      // min value allowed
   uint64   maxValue;      // max value allowed
   uint32   numBins;       // number of histogram bins
};


#if defined(MXUSER_STATS)
/*
 *-----------------------------------------------------------------------------
 *
 * MXUserAddToList --
 *
 *      Add a newly created lock to the list of all userland locks.
 *
 * Results:
 *      The lock is added to the list.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserAddToList(MXUserHeader *header)  // IN:
{
   MXRecLock *listLock = MXUserInternalSingleton(&mxLockMemPtr);

   /* Tolerate a failure. This is too low down to log */
   if (listLock) {
      MXRecLockAcquire(listLock, GetReturnAddress());
      LIST_QUEUE(&header->item, &mxUserLockList);
      MXRecLockRelease(listLock);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserRemoveFromList --
 *
 *      Remove a lock from the list of all userland locks.
 *
 * Results:
 *      The lock is removed from the list.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserRemoveFromList(MXUserHeader *header)  // IN:
{
   MXRecLock *listLock = MXUserInternalSingleton(&mxLockMemPtr);

   /* Tolerate a failure. This is too low down to log */
   if (listLock) {
      MXRecLockAcquire(listLock, GetReturnAddress());
      LIST_DEL(&header->item, &mxUserLockList);
      MXRecLockRelease(listLock);
   }
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserHistoIndex --
 *
 *      Return the index into the histogram bins. This makes use of a
 *      fixed point approximation method.
 *
 * Results:
 *      (uint32) (BINS_PER_DECADE * log10(value))
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static uint32
MXUserHistoIndex(uint64 value)  // IN:
{
   uint32 index;

   if (value == 0) {
      index = 0;
   } else {
      uint32 numerator = 0;
      uint32 denominator = 0;

      LogFixed_Base10(value, &numerator, &denominator); 

      index = (BINS_PER_DECADE * numerator) / denominator;
   }

   return index;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserHistoSetup --
 *
 *      Set up a histogram object using the specified minimum value and
 *      decade coverage. The minimum value must be 1 or a power of 10.
 *
 *      These histograms coverage values from the minimum to
 *      minimum * 10^decades with BINS_PER_DECADE bins for each decade
 *      covered.
 *
 * Results:
 *      NULL  Failure
 *     !NULL  Success (a histogram object pointer is returned)
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

MXUserHisto *
MXUserHistoSetUp(char *typeName,   // type (name) of histogram
                 uint64 minValue,  // IN: ns; 1, 10, 100, 1000...
                 uint32 decades)   // IN: decimal decades to cover from min
{
   MXUserHisto *histo;

   ASSERT(decades > 0);
   ASSERT((minValue != 0) && ((minValue == 1) || ((minValue % 10) == 0)));

   histo = Util_SafeMalloc(sizeof(*histo));

   histo->typeName = Util_SafeStrdup(typeName);
   histo->numBins = BINS_PER_DECADE * decades;
   histo->binData = Util_SafeCalloc(sizeof(uint64), histo->numBins);
   histo->totalSamples = 0;
   histo->minValue = minValue;

   histo->maxValue = histo->minValue;

   while (decades--) {
      histo->maxValue *= 10;
   }

   return histo;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserHistoTearDown --
 *
 *      Tear down a histogram object;
 *
 * Results:
 *      The histogram object is torn down. Don't use it after this.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserHistoTearDown(MXUserHisto *histo)  // IN:
{
   if (histo != NULL) {
      free(histo->typeName);
      free(histo->binData);
      free(histo);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserHistoSample --
 *
 *      Add a sample to the specified histogram.
 *
 *      Out-of-bounds on the low end are summed in bin[0].
 *      Out-of-bounds on the high end are summed in bin[numBins - 1].
 *
 * Results:
 *      As expected.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserHistoSample(MXUserHisto *histo,  // IN/OUT:
                  uint64 durationNS)   // IN:
{
   uint32 index;

   ASSERT(histo);

   histo->totalSamples++;

   if (durationNS < histo->minValue) {
      index = 0;
   } else {
      if (durationNS > histo->maxValue) {
         index = histo->numBins - 1;
      } else {
         index = MXUserHistoIndex(durationNS / histo->minValue);
      }
   }

   ASSERT(index < histo->numBins);

   histo->binData[index]++;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserHistoDump --
 *
 *      Dump the specified histogram for the specified lock.
 *
 * Results:
 *      The histogram is dumped to the statistics log.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserHistoDump(MXUserHisto *histo,    // IN:
                MXUserHeader *header)  // IN:
{
   ASSERT(header);
   ASSERT(histo);

#if defined(MXUSER_STATS)
   if (histo->totalSamples) {
      char *p;
      uint32 i;
      uint32 spaceLeft;

      static uint32 maxLine = 0;
      static char *histoLine = NULL;

      /*
       * Statistics are reported from a single thread. This avoids allocating
       * a potentially large buffer on the stack.
       */ 

      if (maxLine == 0) {
         maxLine = Log_MaxLineLength();  // includes terminating NUL
         ASSERT(maxLine >= 1024);        // assert a rational minimum

         histoLine = Util_SafeMalloc(maxLine);
      }

      i = Str_Sprintf(histoLine, maxLine,
                      "MXUser: h l=%u t=%s min=%"FMT64"u max=%"FMT64"u\n",
                      header->identifier, histo->typeName, histo->minValue,
                      histo->maxValue);

      /*
       * The terminating "\n\0" will be overwritten each time a histogram
       * bin is added to the line. This will ensure that the line is always
       * properly terminated no matter what happens.
       */

      p = &histoLine[i - 1];
      spaceLeft = maxLine - i - 2;

      /* Add as many histogram bins as possible within the line limitations */
      for (i = 0; i < histo->numBins; i++) {
         if (histo->binData[i] != 0) {
            uint32 len;
            char binEntry[32];

            Str_Sprintf(binEntry, sizeof binEntry, " %u-%"FMT64"u\n",
                        i, histo->binData[i]);

            len = strlen(binEntry);

            if (len < spaceLeft) {
               /*
                * Append the bin number, bin count pair to the end of the
                * string. This includes the terminating "\n\0". Update the
                * pointer to the next free place to point to the '\n'. If
                * another entry is made, things work out properly. If not
                * the string is properly terminated as a line.
                */

               Str_Strcpy(p, binEntry, len + 1);
               p += len - 1;
               spaceLeft -= len;
            } else {
               break;
            }
         }
      }

      StatsLog(histoLine);
   }
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserBasicStatsSample --
 *
 *      Add a sample to the "pure" statistics object.
 *
 * Results:
 *      The sample is added.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserBasicStatsSample(MXUserBasicStats *stats,  // IN/OUT:
                       uint64 durationNS)        // IN:
{
   stats->numSamples++;

   if (durationNS < stats->minTime) {
      stats->minTime = durationNS;
   }

   if (durationNS > stats->maxTime) {
      stats->maxTime = durationNS;
   }

   stats->timeSum += durationNS;

   /* Do things in floating point to avoid uint64 overflow */
   stats->timeSquaredSum += ((double) durationNS) * ((double) durationNS);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserBasicStatsSetUp --
 *
 *      Set up the "pure" statistics object.
 *
 * Results:
 *      The statistics are set up.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserBasicStatsSetUp(MXUserBasicStats *stats,  // IN/OUT:
                      char *typeName)           // IN:
{
   stats->typeName = Util_SafeStrdup(typeName);
   stats->numSamples = 0;
   stats->minTime = ~CONST64U(0);
   stats->maxTime = 0;
   stats->timeSum = 0;
   stats->timeSquaredSum = 0.0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserDumpBasicStats --
 *
 *      Dump the basic statistics. This routine may run during concurrent
 *      locking activity so explicit checks are necessary to deal with
 *      jittering data.
 *
 * Results:
 *      Interesting data is added to the statistics log.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserDumpBasicStats(MXUserBasicStats *stats,  // IN:
                     MXUserHeader *header)     // IN:
{
#if defined(MXUSER_STATS)
   uint64 stdDev;

   if (stats->numSamples < 2) {
      /*
       * It's possible to get a request to dump statistics when there
       * aren't any (e.g. a lock has been acquired but never released so
       * there are no "held" statistics yet). Ignore requests to dump
       * statistics when there aren't any.
       */

      if (stats->numSamples == 0) {
         return;
      }

      stdDev = 0;
   } else {
      double num;
      double mean;
      double variance;

      num = (double) stats->numSamples;
      mean = ((double) stats->timeSum) / num;
      variance = (stats->timeSquaredSum - (num*mean*mean)) / (num - 1.0);

      stdDev = (variance < 0.0) ? 0 : (uint64) (sqrt(variance) + 0.5);
   }

   StatsLog("MXUser: e l=%u t=%s c=%"FMT64"u min=%"FMT64"u "
            "max=%"FMT64"u mean=%"FMT64"u sd=%"FMT64"u\n",
            header->identifier, stats->typeName,
            stats->numSamples, stats->minTime, stats->maxTime,
            stats->timeSum/stats->numSamples, stdDev);
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserBasicStatsTearDown --
 *
 *      Tear down an basic statistics object.
 *
 * Results:
 *      The statistics are set up.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserBasicStatsTearDown(MXUserBasicStats *stats)  // IN/OUT:
{
   free(stats->typeName);
   stats->typeName = NULL;
}




/*
 *-----------------------------------------------------------------------------
 *
 * MXUserAcquisitionStatsSetUp --
 *
 *      Set up an acquisition statistics object.
 *
 * Results:
 *      The statistics are set up.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserAcquisitionStatsSetUp(MXUserAcquisitionStats *stats)  // IN/OUT:
{
   MXUserBasicStatsSetUp(&stats->basicStats, MXUSER_STAT_CLASS_ACQUISITION);

   stats->numAttempts = 0;
   stats->numSuccesses = 0;
   stats->numSuccessesContended = 0;
   stats->totalContentionTime = 0;
   stats->successContentionTime = 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserAcquisitionSample --
 *
 *      Track the acquisition specific statistical data.
 *
 * Results:
 *      Much CPU time may be used.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserAcquisitionSample(MXUserAcquisitionStats *stats,  // IN/OUT:
                        Bool wasAcquired,               // IN:
                        Bool wasContended,              // IN:
                        uint64 elapsedTime)             // IN:
{
   stats->numAttempts++;

   if (wasAcquired) {
      stats->numSuccesses++;

      if (wasContended) {
         stats->numSuccessesContended++;
         stats->totalContentionTime += elapsedTime;
         stats->successContentionTime += elapsedTime;
      }

      MXUserBasicStatsSample(&stats->basicStats, elapsedTime);
   } else {
      ASSERT(wasContended);

      stats->totalContentionTime += elapsedTime;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserDumpAcquisitionStats --
 *
 *      Dump the acquisition statistics for the specified lock.
 *
 * Results:
 *      Much CPU time may be used.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserDumpAcquisitionStats(MXUserAcquisitionStats *stats,  // IN:
                           MXUserHeader *header)           // IN:
{
#if defined(MXUSER_STATS)
   if (stats->numAttempts > 0) {
      if (stats->numSuccesses > 0) {
         MXUserDumpBasicStats(&stats->basicStats, header);
      }

      StatsLog("MXUser: ce l=%u a=%"FMT64"u s=%"FMT64"u sc=%"FMT64"u "
               "sct=%"FMT64"u t=%"FMT64"u\n",
               header->identifier,
               stats->numAttempts,
               stats->numSuccesses,
               stats->numSuccessesContended,
               stats->successContentionTime,
               stats->totalContentionTime);
   }
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserAcquisitionStatsTearDown --
 *
 *      Tear down an acquisition statistics object.
 *
 * Results:
 *      The statistics are set up.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserAcquisitionStatsTearDown(MXUserAcquisitionStats *stats)  // IN/OUT:
{
   MXUserBasicStatsTearDown(&stats->basicStats);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserKitchen --
 *
 *      If you can't take the heat, get out of the kitchen! Report on the
 *      heat generated by the specified lock's acquisition statistics.
 *
 * Results:
 *      Data is returned.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserKitchen(MXUserAcquisitionStats *stats,  // IN:
              double *contentionRatio,        // OUT:
              Bool *isHot,                    // OUT:
              Bool *doLog)                    // OUT:
{
   /*
    * How much "heat" is this lock generating?
    */

   if (stats->numAttempts == 0) {
      *contentionRatio = 0.0;
   } else {
      double basic;
      double acquisition;

      /*
       * Contention shows up in two ways - failed attempts to acquire
       * and detected contention while acquiring. Determine which is
       * the largest and use that as the contention ratio for the
       * specified statistics.
       */

      basic = ((double) stats->numAttempts - stats->numSuccesses) /
               ((double) stats->numAttempts);

      acquisition = ((double) stats->numSuccessesContended) /
                     ((double) stats->numSuccesses);

      *contentionRatio = (basic < acquisition) ? acquisition : basic;
   }

   /*
    * Handle the explicit control cases.
    *
    * An mxUserContentionCount value of zero (0) forces all locks to be
    * considered "cold", regardless of their activity.
    *
    * An mxUserContentionCount value of ~((uint64) 0) (all Fs) forces all
    * locks to be considered "hot" regardless of their activity, with the
    * side effect that no logging of "temperature changes" is done.
    */

   if (mxUserContentionCount == 0) {              // never "hot"
      *isHot = FALSE;
      *doLog = FALSE;

      return;
   }

   if (mxUserContentionCount == ~((uint64) 0)) {  // always "hot"; no logging
      *isHot = TRUE;
      *doLog = FALSE;

      return;
   }

   /*
    * Did the thermostat trip?
    */

   ASSERT((mxUserContentionRatio > 0.0) && (mxUserContentionRatio <= 1.0));

   if (*contentionRatio >= mxUserContentionRatio) {  // Yes
      *isHot = TRUE;
      *doLog = TRUE;
   } else {                                          // No
      *doLog = FALSE;
      *isHot = FALSE;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_StatisticsControl --
 *
 *      Specify the settings for automatic "hot locks" operation.
 *
 * Results:
 *      Enhanced statistics will be used or never used, depending on these
 *      values;
 *
 * Side effects:
 *      Unknown...
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_StatisticsControl(double contentionRatio,  // IN:
                         uint64 minCount)         // IN:
{
   ASSERT((contentionRatio > 0.0) && (contentionRatio <= 1.0));

   mxUserContentionRatio = contentionRatio;
   mxUserContentionCount = minCount;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserForceHisto --
 *
 *      Force histogram taking for the specified histogram.
 *
 * Results:
 *      As above.
 *
 * Side effects:
 *      Memory is allocated.
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserForceHisto(Atomic_Ptr *histoPtr,  // IN/OUT:
                 char *typeName,        // IN:
                 uint64 minValue,       // IN:
                 uint32 decades)        // IN:
{
   MXUserHisto *ptr = Atomic_ReadPtr(histoPtr);

   if (ptr == NULL) {
      MXUserHisto *before;

      ptr = MXUserHistoSetUp(typeName, minValue, decades);

      before = (MXUserHisto *) Atomic_ReadIfEqualWritePtr(histoPtr, NULL,
                                                          (void *) ptr);

      if (before) {
         MXUserHistoTearDown(ptr);
      }
   }
}


#if defined(MXUSER_STATS)
/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_PerLockData --
 *
 *      Perform the statistics logging duties.
 *
 *      The dumping is done on active locks so the data is, at best,
 *      approximate. This is OK for statistics builds since things should
 *      be "close enough".
 *
 *      This routine is called from within the statistics collection framework.
 *      It is called, periodically, at the end of each statistical "epoch".
 *
 * Results:
 *      Lots of statistics in statistics (obj/opt/stats) builds.
 *
 * Side effects:
 *      The overhead of the statistics process may affect the timing of
 *      events and expose bugs. Be prepared!
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_PerLockData(void)
{
   MXRecLock *listLock = MXUserInternalSingleton(&mxLockMemPtr);

   if (listLock && MXRecLockTryAcquire(listLock, GetReturnAddress())) {
      ListItem *entry;
      uint32 highestID;
      static uint32 lastReportedID = 0;

      highestID = lastReportedID;

      LIST_SCAN(entry, mxUserLockList) {
         MXUserHeader *header = LIST_CONTAINER(entry, MXUserHeader, item);

         /* Log the ID information for a lock that did exist previously */
         if (header->identifier > lastReportedID) {
            StatsLog("MXUser: n n=%s l=%d r=0x%x\n", header->name,
                     header->identifier, header->rank);

            if (header->identifier > highestID) {
               highestID = header->identifier;
            }
         }

         /*
          * Perform the statistics action for any lock that has one.
          */

         if (header->statsFunc) {
            (*header->statsFunc)(header);
         }
      }

      lastReportedID = highestID;

      MXRecLockRelease(listLock);
   }
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserAllocID --
 *
 *      Allocate and return an MXUser identifier
 *
 *      MXUser identifiers are never recycled.
 *
 * Results:
 *      As above.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

uint32
MXUserAllocID(void)
{
   static Atomic_uint32 firstFreeID = { 1 };  // must start not zero

   return Atomic_FetchAndInc(&firstFreeID);
}