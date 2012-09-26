/*++

Module Name:

    AlignerOptions.h

Abstract:

    Common parameters for running single & paired alignment.

Authors:

    Ravi Pandya, May, 2012

Environment:
`
    User mode service.

Revision History:

    Integrated from SingleAligner.cpp & PairedAligner.cpp

--*/

#pragma once

#include "stdafx.h"
#include "options.h"
#include "Range.h"
#include "Genome.h"
#include "SAM.h"
#include "RangeSplitter.h"

struct AbstractOptions
{
    virtual void usageMessage() = 0;

    virtual bool parse(char** argv, int argc, int& n) = 0;
};

struct AlignerOptions : public AbstractOptions
{
    AlignerOptions(const char* i_commandLine, bool forPairedEnd = false);

    const char         *commandLine;
    const char         *indexDir;
    int                 numThreads;
    Range               maxDist;
    Range               numSeeds;
    Range               maxHits;
    Range               confDiff;
    Range               adaptiveConfDiff;
    bool                computeError;
    bool                bindToProcessors;
    bool                ignoreMismatchedIDs;
    unsigned            selectivity;
    char               *samFileTemplate;
    bool                doAlignerPrefetch;
    char               *inputFilename;
    bool                inputFileIsFASTQ;   // Else SAM
    ReadClippingType    clipping;
    bool                sortOutput;
    unsigned            sortMemory; // total output sorting buffer size in Gb
    unsigned            filterFlags;
    bool                explorePopularSeeds;
    bool                stopOnFirstHit;
	bool				useM;	// Should we generate CIGAR strings using = and X, or using the old-style M?
    AbstractOptions    *extra; // extra options

    void usage();

    virtual void usageMessage();

    virtual bool parse(char** argv, int argc, int& n);

    enum FilterFlags
    {
        FilterUnaligned =           0x0001,
        FilterSingleHit =           0x0002,
        FilterMultipleHits =        0x0004,
    };

    bool passFilter(Read* read, AlignmentResult result);
};
