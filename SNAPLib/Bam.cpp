/*++

Module Name:

    Bam.cpp

Abstract:

    Binary Alignment Map (BAM) file writer and reader.

Environment:

    User mode service.

    BamWriter and BamReader (and their subclasses) aren't thread safe.

--*/

#include "stdafx.h"
#include "SAM.h"
#include "BigAlloc.h"
#include "Compat.h"
#include "Read.h"
#include "Bam.h"
#include "Tables.h"
#include "RangeSplitter.h"
#include "ParallelTask.h"
#include "ReadSupplierQueue.h"
#include "Util.h"
#include "FileFormat.h"
#include "AlignerOptions.h"
#include "exit.h"
#include "VariableSizeMap.h"
#include "PairedAligner.h"
#include "GzipDataWriter.h"

using std::max;
using std::min;
using util::strnchr;

BAMReader::BAMReader(const ReaderContext& i_context) : ReadReader(i_context)
{
}

BAMReader::~BAMReader()
{
}

    bool
BAMReader::getNextReadPair(
    Read *read1,
    Read *read2,
    PairedAlignmentResult *alignmentResult,
    unsigned *mapQ,
    const char **cigar)
{
    return false;
}

    void
BAMReader::init(
    const char *fileName,
    _int64 startingOffset,
    _int64 amountOfFileToProcess)
{
    // todo: integrate supplier models
    // might need up to 3x extra for expanded sequence + quality + cigar data
    data = DataSupplier::GzipBamDefault[false]->getDataReader(MAX_RECORD_LENGTH, 3.0 * DataSupplier::ExpansionFactor);
    if (! data->init(fileName)) {
        fprintf(stderr, "Unable to read file %s\n", fileName);
        soft_exit(1);
    }
    _ASSERT(context.headerBytes > 0);
    reinit(startingOffset, amountOfFileToProcess);
    if ((size_t) startingOffset < context.headerBytes) {
        char* p;
        _int64 valid, start;
        bool ok = data->getData(&p, &valid, &start);
        if (! ok) {
            fprintf(stderr, "failure reading file %s\n", fileName);
            soft_exit(1);
        }
        _int64 skip = context.headerBytes - startingOffset;
        _ASSERT(skip < valid);
        data->advance(skip);
        if (skip > start) {
            data->nextBatch();
            data->getData(&p, &valid, &start);
        }
    }
}

    void
BAMReader::readHeader(
    const char* fileName,
    ReaderContext& context)
{
    _ASSERT(context.header == NULL);
    DataReader* data = DataSupplier::GzipBamDefault[false]->getDataReader(MAX_RECORD_LENGTH, 3.0 * DataSupplier::ExpansionFactor);
    if (! data->init(fileName)) {
        fprintf(stderr, "Unable to read file %s\n", fileName);
        soft_exit(1);
    }
    _int64 headerSize = 1024 * 1024; // 1M header max
    char* buffer = data->readHeader(&headerSize);
    BAMHeader* header = (BAMHeader*) buffer;
    if (header->magic != BAMHeader::BAM_MAGIC) {
        fprintf(stderr, "BAMReader: Not a valid BAM file\n");
        soft_exit(1);
    }
    _int64 textHeaderSize = header->l_text;
    if (!SAMReader::parseHeader(fileName, header->text(), header->text() + textHeaderSize, context.genome, &textHeaderSize, &context.headerMatchesIndex)) {
        fprintf(stderr,"BAMReader: failed to parse header on '%s'\n",fileName);
        soft_exit(1);
    }
    int n_ref = header->n_ref();
    BAMHeaderRefSeq* refSeq = header->firstRefSeq();
    for (int i = 0; i < n_ref; i++, refSeq = refSeq->next()) {
        // just advance
    }

    char* p = new char[textHeaderSize + 1];
    memcpy(p, header->text(), textHeaderSize);
    p[textHeaderSize] = 0;
    context.header = p;
    context.headerLength = textHeaderSize;
    context.headerBytes = (char*) refSeq - buffer;

    delete data;
}

    BAMReader*
BAMReader::create(
    const char *fileName,
    _int64 startingOffset,
    _int64 amountOfFileToProcess,
    const ReaderContext& context)
{
    BAMReader* reader = new BAMReader(context);
    reader->init(fileName, startingOffset, amountOfFileToProcess);
    return reader;
}

    void
BAMReader::reinit(
    _int64 startingOffset,
    _int64 amountOfFileToProcess)
{
    data->reinit(startingOffset, amountOfFileToProcess);
    extraOffset = 0;
}

    ReadSupplierGenerator *
BAMReader::createReadSupplierGenerator(
    const char *fileName,
    int numThreads,
    const ReaderContext& context)
{
    BAMReader* reader = create(fileName, 0, 0, context);
    ReadSupplierQueue* queue = new ReadSupplierQueue((ReadReader*)reader);
    queue->startReaders();
    return queue;
}

    PairedReadSupplierGenerator *
BAMReader::createPairedReadSupplierGenerator(
    const char *fileName,
    int numThreads,
    const ReaderContext& context,
    int matchBufferSize)
{
    BAMReader* reader = create(fileName, 0, 0, context);
    PairedReadReader* matcher = PairedReadReader::PairMatcher(reader, false);
    ReadSupplierQueue* queue = new ReadSupplierQueue(matcher);
    queue->startReaders();
    return queue;
}

const char* BAMAlignment::CodeToSeq = "=ACMGRSVTWYHKDBN";
_uint8 BAMAlignment::SeqToCode[256];
const char* BAMAlignment::CodeToCigar = "MIDNSHP=X";
_uint8 BAMAlignment::CigarToCode[256];
_uint8 BAMAlignment::CigarCodeToRefBase[9] = {1, 0, 1, 1, 0, 0, 1, 1, 1};

BAMAlignment::_init BAMAlignment::_init_;

    void
BAMAlignment::decodeSeq(
    char* o_sequence,
    _uint8* nibbles,
    int bases)
{
    for (int i = 0; i < bases; i++) {
        int n = (i & 1) ? *nibbles & 0xf : *nibbles >> 4;
        nibbles += i & 1;
        *o_sequence++ = BAMAlignment::CodeToSeq[n];
    }
}

    void
BAMAlignment::decodeQual(
    char* o_qual,
    char* quality,
    int bases)
{
    for (int i = 0; i < bases; i++) {
        char q = quality[i];
        o_qual[i] = q < 0 || q >= 64 ? '!' : q + '!';
    }
}

    bool
BAMAlignment::decodeCigar(
    char* o_cigar,
    int cigarSize,
    _uint32* cigar,
    int ops)
{
    int i = 0;
    _uint32 lastOp = 99999;
    while (ops > 0 && i < cigarSize - 11) { // 9 decimal digits (28 bits) + 1 cigar char + null terminator
        i += sprintf(o_cigar + i, "%u", *cigar >> 4);
        _ASSERT((*cigar & 0xf) <= 8);
        _uint32 op = *cigar & 0xf;
        o_cigar[i++] = BAMAlignment::CodeToCigar[op];
        _ASSERT(op != lastOp);
        lastOp = op;
        ops--;
        cigar++;
    }
    o_cigar[i++] = 0;
    return ops == 0;
}

    void
BAMAlignment::encodeSeq(
    _uint8* encoded,
    char* ascii,
    int length)
{
    _uint8* p = encoded;
    for (int i = 0; i + 1 < length; i += 2) {
        *p++ = (BAMAlignment::SeqToCode[ascii[i]] << 4) | BAMAlignment::SeqToCode[ascii[i+1]];
    }
    if (length % 2) {
        *p = BAMAlignment::SeqToCode[ascii[length - 1]] << 4;
    }
}

    int
BAMAlignment::l_ref()
{
    if (FLAG & SAM_UNMAPPED) {
        return 0;
    }
    if (n_cigar_op == 0) {
        return l_seq;
    }
    _uint32* p = cigar();
    int len = 0;
    static const int op_ref[16] = {1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < n_cigar_op; i++) {
        _uint32 op = *p++;
        len += op_ref[(op & 15)] * (op >> 4);
    }
    return len;
}

// static initializer
BAMAlignment::_init::_init()
{
    memset(SeqToCode, 0, 256);
    for (int i = 1; i < 16; i++) {
        SeqToCode[CodeToSeq[i]] = i;
    }
    memset(CigarToCode, 0, 256);
    for (int i = 1; i < 9; i++) {
        CigarToCode[CodeToCigar[i]] = i;
    }
}

    int
BAMAlignment::reg2bin(
    int beg,
    int end)
{
    --end;
    if (beg>>14 == end>>14) return ((1<<15)-1)/7 + (beg>>14);
    if (beg>>17 == end>>17) return ((1<<12)-1)/7 + (beg>>17);
    if (beg>>20 == end>>20) return ((1<<9)-1)/7 + (beg>>20);
    if (beg>>23 == end>>23) return ((1<<6)-1)/7 + (beg>>23);
    if (beg>>26 == end>>26) return ((1<<3)-1)/7 + (beg>>26);
    return 0;
}

    int
BAMAlignment::reg2bins(
    int beg,
    int end,
    _uint16* list)
{
    int i = 0, k;
    --end;
    list[i++] = 0;
    for (k = 1 + (beg>>26); k <= 1 + (end>>26); ++k) list[i++] = k;
    for (k = 9 + (beg>>23); k <= 9 + (end>>23); ++k) list[i++] = k;
    for (k = 73 + (beg>>20); k <= 73 + (end>>20); ++k) list[i++] = k;
    for (k = 585 + (beg>>17); k <= 585 + (end>>17); ++k) list[i++] = k;
    for (k = 4681 + (beg>>14); k <= 4681 + (end>>14); ++k) list[i++] = k;
    return i;
}

#ifdef VALIDATE_BAM
    void
BAMAlignment::validate()
{
    _ASSERT(block_size < 0x100000); // sanity check, should be <1MB!
    _ASSERT(size(l_read_name, n_cigar_op, l_seq, 0) <= block_size + sizeof(block_size));
    _ASSERT(refID >= -1 && refID <= (int) 0x100000);
    // todo: validate bin, requires more info
    _ASSERT(MAPQ <= 80 || MAPQ == 255);
    _ASSERT(FLAG <= 0x7ff);
    _ASSERT(next_refID >= -1 && refID <= (int) 0x100000);
    for (char* p = read_name(); p < read_name() + l_read_name - 1; p++) {
        _ASSERT(*p >= ' ' && *p <= '~');
    }
    _ASSERT(read_name()[l_read_name - 1] == 0);
    // can't validate seq, all values are valid (though some are unlikely!)
    char* q = qual();
    for (int i = 0; i < l_seq; i++) {
        _ASSERT(q[i] >= -1 && q[i] <= 80);
    }
    BAMAlignAux* aux = firstAux();
    for (; (char*)aux - (char*)firstAux() < auxLen(); aux = aux->next()) {
        _ASSERT(aux->tag[0] >= ' ' && aux->tag[0] <= '~' && aux->tag[1] >= ' ' && aux->tag[1] <= '~');
        _ASSERT(strchr("AcCsSiIfZHB", aux->val_type) != NULL);
    }
    _ASSERT((char*) aux - (char*) firstAux() == auxLen());
}
#endif

    bool
BAMReader::getNextRead(
    Read *read,
    AlignmentResult *alignmentResult,
    unsigned *genomeLocation,
    bool *isRC,
    unsigned *mapQ,
    unsigned *flag,
    bool ignoreEndOfRange,
    const char **cigar)
{
    unsigned local_flag;
    if (NULL == flag) {
        flag = &local_flag;
    }

    do {
        char* buffer;
        _int64 bytes;
        if (! data->getData(&buffer, &bytes)) {
            data->nextBatch();
            if (! data->getData(&buffer, &bytes)) {
                return false;
            }
            extraOffset = 0;
        }
        BAMAlignment* bam = (BAMAlignment*) buffer;
        if ((_uint64)bytes < sizeof(bam->block_size) || (_uint64)bytes < bam->size()) {
            fprintf(stderr, "Unexpected end of BAM file at %lld\n", data->getFileOffset());
            soft_exit(1);
        }
        data->advance(bam->size());
        size_t lineLength;
        getReadFromLine(context.genome, buffer, buffer + bytes, read, alignmentResult, genomeLocation,
            isRC, mapQ, &lineLength, flag, cigar, context.clipping);
        unsigned auxLen = bam->auxLen();
        read->setReadGroup(context.defaultReadGroup);
        if (auxLen > 0) {
            read->setAuxiliaryData((char*) bam->firstAux(), auxLen);
            for (BAMAlignAux* aux = bam->firstAux(); aux < bam->endAux(); aux = aux->next()) {
                if (aux->val_type == 'Z' && aux->tag[0] == 'R' && aux->tag[1] == 'G') {
                    read->setReadGroup(READ_GROUP_FROM_AUX);
                    break;
                }
            }
        }
    } while (context.ignoreSecondaryAlignments && (*flag & SAM_SECONDARY));
    return true;
}

    void
BAMReader::getReadFromLine(
    const Genome *genome,
    char *line,
    char *endOfBuffer,
    Read *read,
    AlignmentResult *alignmentResult,
    unsigned *out_genomeLocation,
    bool *isRC,
    unsigned *mapQ,
    size_t *lineLength,
    unsigned *flag,
    const char **cigar,
    ReadClippingType clipping)
{
    _ASSERT(endOfBuffer - line >= sizeof(BAMHeader));
    BAMAlignment* bam = (BAMAlignment*) line;
    _ASSERT((size_t)(endOfBuffer - line) >= bam->size());
    bam->validate();

    unsigned genomeLocation = bam->getLocation(genome);

    if (NULL != out_genomeLocation) {
        _ASSERT(-1 <= bam->refID && bam->refID < (int)genome->getNumContigs());
        *out_genomeLocation = genomeLocation;
    }


    char* cigarBuffer = getExtra(MAX_SEQ_LENGTH);
    if (! BAMAlignment::decodeCigar(cigarBuffer, MAX_SEQ_LENGTH, bam->cigar(), bam->n_cigar_op)) {
        cigarBuffer = ""; // todo: fail?
    }

    if (NULL != cigar) {
        *cigar = cigarBuffer;
    }

    if (NULL != read) {
        _ASSERT(bam->l_seq < MAX_SEQ_LENGTH);
		char* seqBuffer = getExtra(bam->l_seq);
        char* qualBuffer = getExtra(bam->l_seq);
        BAMAlignment::decodeSeq(seqBuffer, bam->seq(), bam->l_seq);
        BAMAlignment::decodeQual(qualBuffer, bam->qual(), bam->l_seq);

        unsigned originalFrontClipping, originalBackClipping, originalFrontHardClipping, originalBackHardClipping;
        Read::computeClippingFromCigar(cigarBuffer, &originalFrontClipping, &originalBackClipping, &originalFrontHardClipping, &originalBackHardClipping);

        read->init(bam->read_name(), bam->l_read_name - 1, seqBuffer, qualBuffer, bam->l_seq, genomeLocation, bam->MAPQ, bam->FLAG,
            originalFrontClipping, originalBackClipping, originalFrontHardClipping, originalBackHardClipping);
        read->setBatch(data->getBatch());
        if (bam->FLAG & SAM_REVERSE_COMPLEMENT) {
            read->becomeRC();
        }
        read->clip(clipping);
    }

    if (NULL != alignmentResult) {
        _ASSERT(bam->FLAG & SAM_UNMAPPED || bam->refID >= 0);
        *alignmentResult = bam->FLAG & SAM_UNMAPPED ? NotFound : SingleHit; // todo: look at MAPQ?
    }

    if (NULL != isRC) {
        *isRC = (bam->FLAG & SAM_REVERSE_COMPLEMENT) != 0;
    }

    if (NULL != mapQ) {
        *mapQ = bam->MAPQ;
    }

    if (NULL != flag) {
        *flag = bam->FLAG;
    }


}

    char*
BAMReader::getExtra(
    _int64 bytes)
{
    char* extra;
    _int64 limit;
    data->getExtra(&extra, &limit);
    _ASSERT(extra != NULL && bytes >= 0 && limit - extraOffset >= bytes);
    if (limit - extraOffset < bytes) {
        fprintf(stderr, "error: not enough space for expanding BAM file - increase expansion factor, currently -xf %.1f\n", DataSupplier::ExpansionFactor);
        soft_exit(1);
    }
    char* result = extra + extraOffset;
    extraOffset += max((_int64) 0, bytes);
    return result;
}


class BAMFormat : public FileFormat
{
public:
    BAMFormat(bool i_useM) : useM(i_useM) {}

    virtual bool isFormatOf(const char* filename) const;

    virtual void getSortInfo(const Genome* genome, char* buffer, _int64 bytes, unsigned* o_location, unsigned* o_readBytes, int* o_refID, int* o_pos) const;

    virtual ReadWriterSupplier* getWriterSupplier(AlignerOptions* options, const Genome* genome) const;

    virtual bool writeHeader(
        const ReaderContext& context, char *header, size_t headerBufferSize, size_t *headerActualSize,
        bool sorted, int argc, const char **argv, const char *version, const char *rgLine) const;

    virtual bool writeRead(
        const Genome * genome, LandauVishkinWithCigar * lv, char * buffer, size_t bufferSpace,
        size_t * spaceUsed, size_t qnameLen, Read * read, AlignmentResult result,
        int mapQuality, unsigned genomeLocation, Direction direction,
        bool hasMate = false, bool firstInPair = false, Read * mate = NULL,
        AlignmentResult mateResult = NotFound, unsigned mateLocation = 0, Direction mateDirection = FORWARD) const;

private:

    static int computeCigarOps(const Genome * genome, LandauVishkinWithCigar * lv,
        char * cigarBuf, int cigarBufLen,
        const char * data, unsigned dataLength, unsigned basesClippedBefore, unsigned extraBasesClippedBefore, unsigned basesClippedAfter,
        unsigned extraBasesClippedAfter,     unsigned frontHardClipping, unsigned backHardClipping,
        unsigned genomeLocation, bool isRC, bool useM, int * editDistance);

    const bool useM;
};

const FileFormat* FileFormat::BAM[] = { new BAMFormat(false), new BAMFormat(true) };

    bool
BAMFormat::isFormatOf(
    const char* filename) const
{
    return util::stringEndsWith(filename, ".bam");
}

    void
BAMFormat::getSortInfo(
    const Genome* genome,
    char* buffer,
    _int64 bytes,
    unsigned* o_location,
	unsigned* o_readBytes,
	int* o_refID,
	int* o_pos) const
{
    BAMAlignment* bam = (BAMAlignment*) buffer;
    _ASSERT((size_t) bytes >= sizeof(BAMAlignment) && bam->size() <= (size_t) bytes && bam->refID < genome->getNumContigs());
	if (o_location != NULL) {
		if (bam->refID < 0 || bam->refID >= genome->getNumContigs() || bam->pos < 0) {
			if (bam->next_refID < 0 || bam->next_refID > genome->getNumContigs() || bam->next_pos < 0) {
				*o_location = UINT32_MAX;
			} else {
				*o_location = genome->getContigs()[bam->next_refID].beginningOffset + bam->next_pos;
			}
		} else {
			*o_location = genome->getContigs()[bam->refID].beginningOffset + bam->pos;
		}
	}
	if (o_readBytes != NULL) {
		*o_readBytes = (unsigned) bam->size();
	}
	if (o_refID != NULL) {
		*o_refID = bam->refID;
	}
	if (o_pos != NULL) {
		*o_pos = bam->pos;
	}
}

    ReadWriterSupplier*
BAMFormat::getWriterSupplier(
    AlignerOptions* options,
    const Genome* genome) const
{
    DataWriterSupplier* dataSupplier;
    GzipWriterFilterSupplier* gzipSupplier =
        DataWriterSupplier::gzip(true, BAM_BLOCK, max(1, options->numThreads - 1), false, options->sortOutput);
        // (leave a thread free for main, and let OS map threads to cores to allow system IO etc.)
    if (options->sortOutput) {
        size_t len = strlen(options->outputFileTemplate);
        // todo: this is going to leak, but there's no easy way to free it, and it's small...
        char* tempFileName = (char*) malloc(5 + len);
        strcpy(tempFileName, options->outputFileTemplate);
        strcpy(tempFileName + len, ".tmp");
        // todo: make markDuplicates optional?
        DataWriter::FilterSupplier* filters = gzipSupplier;
        if (! options->noDuplicateMarking) {
            filters = DataWriterSupplier::markDuplicates(genome)->compose(filters);
        }
        if (! options->noIndex) {
            char* indexFileName = (char*) malloc(5 + len);
            strcpy(indexFileName, options->outputFileTemplate);
            strcpy(indexFileName + len, ".bai");
            filters = DataWriterSupplier::bamIndex(indexFileName, genome, gzipSupplier)->compose(filters);
        }
        dataSupplier = DataWriterSupplier::sorted(this, genome, tempFileName,
            options->sortMemory * (1ULL << 30),
            options->numThreads, options->outputFileTemplate, filters,
            FileEncoder::gzip(gzipSupplier, options->numThreads, options->bindToProcessors));
    } else {
        dataSupplier = DataWriterSupplier::create(options->outputFileTemplate, gzipSupplier);
    }
    return ReadWriterSupplier::create(this, dataSupplier, genome);
}

    bool
BAMFormat::writeHeader(
    const ReaderContext& context,
    char *header,
    size_t headerBufferSize,
    size_t *headerActualSize,
    bool sorted,
    int argc,
    const char **argv,
    const char *version,
    const char *rgLine) const
{
    if (headerBufferSize < BAMHeader::size(0)) {
        return false;
    }
    size_t cursor = 0;
    BAMHeader* bamHeader = (BAMHeader*) header;
    bamHeader->magic = BAMHeader::BAM_MAGIC;
    size_t samHeaderSize;
    bool ok = FileFormat::SAM[0]->writeHeader(context, bamHeader->text(), headerBufferSize - BAMHeader::size(0), &samHeaderSize,
        sorted, argc, argv, version, rgLine);
    if (! ok) {
        return false;
    }
    bamHeader->l_text = (int)samHeaderSize;
    cursor = BAMHeader::size((int)samHeaderSize);

    // Write a RefSeq record for each chromosome / contig in the genome
    // todo: handle null genome index case - reparse header & translate into BAM
	if (context.genome != NULL) {
		const Genome::Contig *contigs = context.genome->getContigs();
		int numContigs = context.genome->getNumContigs();
		bamHeader->n_ref() = numContigs;
		BAMHeaderRefSeq* refseq = bamHeader->firstRefSeq();
		unsigned genomeLen = context.genome->getCountOfBases();
		for (int i = 0; i < numContigs; i++) {
			int len = (int)strlen(contigs[i].name) + 1;
			cursor += BAMHeaderRefSeq::size(len);
			if (cursor > headerBufferSize) {
				return false;
			}
			refseq->l_name = len;
			memcpy(refseq->name(), contigs[i].name, len);
			unsigned start = contigs[i].beginningOffset;
            unsigned end = ((i + 1 < numContigs) ? contigs[i+1].beginningOffset : genomeLen) - context.genome->getChromosomePadding();
            refseq->l_ref() = end - start;
			refseq = refseq->next();
			_ASSERT((char*) refseq - header == cursor);
		}
	}
    *headerActualSize = cursor;
    return true;
}

    bool
BAMFormat::writeRead(
    const Genome * genome,
    LandauVishkinWithCigar * lv,
    char * buffer,
    size_t bufferSpace,
    size_t * spaceUsed,
    size_t qnameLen,
    Read * read,
    AlignmentResult result,
    int mapQuality,
    unsigned genomeLocation,
    Direction direction,
    bool hasMate,
    bool firstInPair,
    Read * mate,
    AlignmentResult mateResult,
    unsigned mateLocation,
    Direction mateDirection) const
{
    const int MAX_READ = MAX_READ_LENGTH;
    const int cigarBufSize = MAX_READ;
    _uint32 cigarBuf[cigarBufSize];

    int flags = 0;
    const char *contigName = "*";
    int contigIndex = -1;
    unsigned positionInContig = 0;
    int cigarOps = 0;
    const char *mateContigName = "*";
    int mateContigIndex = -1;
    unsigned matePositionInContig = 0;
    _int64 templateLength = 0;

    char data[MAX_READ];
    char quality[MAX_READ];

    const char* clippedData;
    unsigned fullLength;
    unsigned clippedLength;
    unsigned basesClippedBefore;
    unsigned extraBasesClippedBefore;
    unsigned basesClippedAfter;
    unsigned extraBasesClippedAfter;
    int editDistance;

    if (! SAMFormat::createSAMLine(genome, lv, data, quality, MAX_READ, contigName, contigIndex,
        flags, positionInContig, mapQuality, mateContigName, mateContigIndex, matePositionInContig, templateLength,
        fullLength, clippedData, clippedLength, basesClippedBefore, basesClippedAfter,
        qnameLen, read, result, genomeLocation, direction, useM,
        hasMate, firstInPair, mate, mateResult, mateLocation, mateDirection,
        &extraBasesClippedBefore, &extraBasesClippedAfter))
    {
        return false;
    }
    if (genomeLocation != InvalidGenomeLocation) {
        cigarOps = computeCigarOps(genome, lv, (char*) cigarBuf, cigarBufSize * sizeof(_uint32),
                                   clippedData, clippedLength, basesClippedBefore, extraBasesClippedBefore, basesClippedAfter, extraBasesClippedAfter,
                                   read->getOriginalFrontHardClipping(), read->getOriginalBackHardClipping(),
                                   genomeLocation, direction == RC, useM, &editDistance);
    }

    // Write the BAM entry
    unsigned auxLen;
    bool auxSAM;
    char* aux = read->getAuxiliaryData(&auxLen, &auxSAM);
    static bool warningPrinted = false;
    bool translateReadGroupFromSAM = false;
    if (aux != NULL && auxSAM) {
        if (! warningPrinted) {
            warningPrinted = true;
            fprintf(stderr, "warning: translating optional data from SAM->BAM is not yet implemented, optional data will not appear in BAM\n");
        }
        if (read->getReadGroup() == READ_GROUP_FROM_AUX) {
            for (char* p = aux; p != NULL && p < aux + auxLen; p = SAMReader::skipToBeyondNextRunOfSpacesAndTabs(p, aux + auxLen)) {
                if (strncmp(p, "RG:Z:", 5) == 0) {
                    size_t fieldLen;
                    SAMReader::skipToBeyondNextRunOfSpacesAndTabs(p, aux + auxLen, &fieldLen);
                    aux = p;
                    auxLen = (unsigned) fieldLen - 1;
                    translateReadGroupFromSAM = true;
                    break;
                }
            }
        }
        if (! translateReadGroupFromSAM) {
            aux = NULL;
            auxLen = 0;
        }
    }
    size_t bamSize = BAMAlignment::size((unsigned)qnameLen + 1, cigarOps, fullLength, auxLen);
    if (read->getReadGroup() != NULL && read->getReadGroup() != READ_GROUP_FROM_AUX) {
        bamSize += 4 + strlen(read->getReadGroup());
    }
    bamSize += 4; // NM:C field
    bamSize += strlen("PGZSNAP") + 1; // PG field
    if (bamSize > bufferSpace) {
        return false;
    }
    BAMAlignment* bam = (BAMAlignment*) buffer;
    bam->block_size = (int)bamSize - 4;
    bam->refID = contigIndex;
    bam->pos = positionInContig - 1;

    if (qnameLen > 254) {
        fprintf(stderr, "BAM format: QNAME field must be less than 254 characters long, instead it's %lld\n", qnameLen);
        soft_exit(1);
    }
    bam->l_read_name = (_uint8)qnameLen + 1;
    bam->MAPQ = mapQuality;
    int refLength = cigarOps > 0 ? 0 : fullLength;
    for (int i = 0; i < cigarOps; i++) {
        refLength += BAMAlignment::CigarCodeToRefBase[cigarBuf[i] & 0xf] * (cigarBuf[i] >> 4);
    }
    bam->bin = genomeLocation != InvalidGenomeLocation ? BAMAlignment::reg2bin(positionInContig-1, positionInContig-1 + refLength) :
		// unmapped is at mate's position, length 1
		mateLocation != InvalidGenomeLocation ? BAMAlignment::reg2bin(matePositionInContig-1, matePositionInContig) :
		// otherwise at -1, length 1
		BAMAlignment::reg2bin(-1, 0);
    bam->n_cigar_op = cigarOps;
    bam->FLAG = flags;
    bam->l_seq = fullLength;
    bam->next_refID = mateContigIndex;
    bam->next_pos = matePositionInContig - 1;
    bam->tlen = (int)templateLength;
    memcpy(bam->read_name(), read->getId(), qnameLen);
    bam->read_name()[qnameLen] = 0;
    memcpy(bam->cigar(), cigarBuf, cigarOps * 4);
    BAMAlignment::encodeSeq(bam->seq(), data, fullLength);
    for (unsigned i = 0; i < fullLength; i++) {
        quality[i] -= '!';
    }
    memcpy(bam->qual(), quality, fullLength);
    if (aux != NULL && auxLen > 0) {
        if (! translateReadGroupFromSAM) {
            memcpy(bam->firstAux(), aux, auxLen);
        } else {
            // hack, build just RG field from SAM opt field
            BAMAlignAux* auxData = bam->firstAux();
            auxData->tag[0] = 'R';
            auxData->tag[1] = 'G';
            auxData->val_type = 'Z';
            memcpy(auxData->value(), aux + 5, auxLen - 4);
            ((char*)auxData->value())[auxLen-1] = 0;
        }
    }
    // RG
    if (read->getReadGroup() != NULL && read->getReadGroup() != READ_GROUP_FROM_AUX) {
        BAMAlignAux* rg = (BAMAlignAux*) (auxLen + (char*) bam->firstAux());
        rg->tag[0] = 'R'; rg->tag[1] = 'G'; rg->val_type = 'Z';
        strcpy((char*) rg->value(), read->getReadGroup());
        auxLen += (unsigned) rg->size();
    }
    // PG
    BAMAlignAux* pg = (BAMAlignAux*) (auxLen + (char*) bam->firstAux());
    pg->tag[0] = 'P'; pg->tag[1] = 'G'; pg->val_type = 'Z';
    strcpy((char*) pg->value(), "SNAP");
    auxLen += (unsigned) pg->size();
    // NM
    BAMAlignAux* nm = (BAMAlignAux*) (auxLen + (char*) bam->firstAux());
    nm->tag[0] = 'N'; nm->tag[1] = 'M'; nm->val_type = 'C';
    *(_uint8*)nm->value() = (_uint8)editDistance;
    auxLen += (unsigned) nm->size();

    if (NULL != spaceUsed) {
        *spaceUsed = bamSize;
    }
    bam->validate();
    return true;
}

// Compute the CIGAR edit sequence operations in BAM format for a read against a given genome location
// Returns number of operations (or 0 if there was a problem)
    int
BAMFormat::computeCigarOps(
    const Genome *              genome,
    LandauVishkinWithCigar *    lv,
    char *                      cigarBuf,
    int                         cigarBufLen,
    const char *                data,
    unsigned                    dataLength,
    unsigned                    basesClippedBefore,
    unsigned                    extraBasesClippedBefore,
    unsigned                    basesClippedAfter,
    unsigned                    extraBasesClippedAfter,
    unsigned                    frontHardClipping,
    unsigned                    backHardClipping,
    unsigned                    genomeLocation,
    bool                        isRC,
	bool						useM,
    int *                       editDistance
)
{
    //
    // Apply the extra clipping.
    //
    genomeLocation += extraBasesClippedBefore;
    data += extraBasesClippedBefore;
    dataLength -= extraBasesClippedBefore;

    unsigned clippingWordsBefore = ((basesClippedBefore + extraBasesClippedBefore > 0) ? 1 : 0) + ((frontHardClipping > 0) ? 1 : 0);
    unsigned clippingWordsAfter = ((basesClippedAfter + extraBasesClippedAfter > 0) ? 1 : 0) + ((backHardClipping > 0) ? 1 : 0);

    const char *reference = genome->getSubstring(genomeLocation, dataLength);
    int used;
    if (NULL != reference) {
        *editDistance = lv->computeEditDistanceNormalized(
                            reference,
                            dataLength - extraBasesClippedAfter,
                            data,
                            dataLength - extraBasesClippedAfter,
                            MAX_K - 1,
                            cigarBuf + 4 * clippingWordsBefore,
                            cigarBufLen - 4 * (clippingWordsBefore + clippingWordsAfter),
						    useM, BAM_CIGAR_OPS, &used);
    } else {
        //
        // Fell off the end of the chromosome.
        //
        return 0;
    }

    if (*editDistance == -2) {
        fprintf(stderr, "WARNING: computeEditDistance returned -2; cigarBuf may be too small\n");
        return 0;
    } else if (*editDistance == -1) {
        static bool warningPrinted = false;
        if (!warningPrinted) {
            fprintf(stderr, "WARNING: computeEditDistance returned -1; this shouldn't happen\n");
            warningPrinted = true;
        }
        return 0;
    } else {
        //
        // If we have hard clipping, add in the cigar string for it.
        //
        if (frontHardClipping > 0) {
            *(_uint32*)cigarBuf = (frontHardClipping << 4) | BAMAlignment::CigarToCode['H'];
            used += 4;
        }
        // Add some CIGAR instructions for soft-clipping if we've ignored some bases in the read.
        if (basesClippedBefore + extraBasesClippedBefore > 0) {
            *((_uint32*)cigarBuf + ((frontHardClipping > 0) ? 1 : 0))  = ((basesClippedBefore + extraBasesClippedBefore) << 4) | BAMAlignment::CigarToCode['S'];
            used += 4;
        }
        if (basesClippedAfter + extraBasesClippedAfter > 0) {
            *(_uint32*)(cigarBuf + used) = ((basesClippedAfter + extraBasesClippedAfter) << 4) | BAMAlignment::CigarToCode['S'];
            used += 4;
        }

        if (backHardClipping > 0) {
            *(_uint32*)(cigarBuf + used) = (backHardClipping << 4) | BAMAlignment::CigarToCode['H'];
            used += 4;
        }
        return used / 4;
    }
}

class BAMFilter : public DataWriter::Filter
{
public:
    BAMFilter(DataWriter::FilterType i_type) : Filter(i_type), offsets(1000), header(false) {}

    virtual ~BAMFilter() {}

	virtual void inHeader(bool flag)
	{ header = flag; }

    virtual void onAdvance(DataWriter* writer, size_t batchOffset, char* data, unsigned bytes, unsigned location);

    virtual size_t onNextBatch(DataWriter* writer, size_t offset, size_t bytes);

protected:
    virtual void onRead(BAMAlignment* bam, size_t fileOffset, int batchIndex) = 0;

    BAMAlignment* getRead(size_t fileOffset);

    BAMAlignment* getNextRead(BAMAlignment* read, size_t* o_fileOffset = NULL);

    BAMAlignment* tryFindRead(size_t offset, size_t endOffset, const char* id, size_t* o_offset);

private:
    bool header;
    VariableSizeVector<size_t> offsets;
    DataWriter* currentWriter;
    char* currentBuffer;
    size_t currentBufferBytes; // # of valid bytes
    size_t currentOffset; // logical file offset of beginning of current buffer
};

    size_t
BAMFilter::onNextBatch(
    DataWriter* writer,
    size_t offset,
    size_t bytes)
{
    bool ok = writer->getBatch(-1, &currentBuffer, NULL, NULL, NULL, &currentBufferBytes, &currentOffset);
    _ASSERT(ok);
    currentWriter = writer;
    int index = 0;
    for (VariableSizeVector<size_t>::iterator i = offsets.begin(); i != offsets.end(); i++) {
        onRead((BAMAlignment*) (currentBuffer + *i), currentOffset + *i, index++);
    }
    offsets.clear();
    currentWriter = NULL;
    currentBuffer = NULL;
    currentBufferBytes = 0;
    currentOffset = 0;
    return bytes;
}

    void
BAMFilter::onAdvance(
    DataWriter* writer,
    size_t batchOffset,
    char* data,
    unsigned bytes,
    unsigned location)
{
    if (! header) {
        offsets.push_back(batchOffset);
    }
}

    BAMAlignment*
BAMFilter::getRead(
    size_t offset)
{
    if (offset >= currentOffset && offset < currentOffset + currentBufferBytes) {
        return (BAMAlignment*) (currentBuffer + (offset - currentOffset));
    }
    for (int i = -2; ; i--) {
        char* buffer;
        size_t bufferFileOffset, bufferUsed; // logical
        if (! currentWriter->getBatch(i, &buffer, NULL, NULL, NULL, &bufferUsed, &bufferFileOffset)) {
            break;
        }
        if (offset >= bufferFileOffset && offset < bufferFileOffset + bufferUsed) {
            return (BAMAlignment*) (buffer + (offset - bufferFileOffset));
        }
    }
    return NULL;
}

    BAMAlignment*
BAMFilter::getNextRead(
    BAMAlignment* bam,
    size_t* io_offset)
{
    char* p = (char*) bam;
    size_t size = bam->size();
    size_t oldOffset = *io_offset;
    *io_offset += size;
    if (p >= currentBuffer && p < currentBuffer + currentBufferBytes) {
        p += bam->size();
        if (p >= currentBuffer + currentBufferBytes) {
            return NULL;
        }
        _ASSERT(*io_offset == currentOffset + (p - currentBuffer));
        _ASSERT(((BAMAlignment*)p)->refID >= -1);
        return (BAMAlignment*) p;
    }
    for (int i = -2; ; i--) {
        char* buffer;
        size_t bufferOffset, bufferUsed; // logical
        if (! currentWriter->getBatch(i, &buffer, NULL, NULL, NULL, &bufferUsed, &bufferOffset)) {
            break;
        }
        if (p >= buffer && p < buffer+ bufferUsed) {
            p += size;
            _ASSERT(*io_offset == bufferOffset + (p - buffer));
            return p < buffer + bufferUsed? (BAMAlignment*) p : getRead(*io_offset);
        }
    }
    return NULL;
}

    BAMAlignment*
BAMFilter::tryFindRead(
    size_t offset,
    size_t endOffset,
    const char* id,
    size_t* o_offset)
{
    BAMAlignment* bam = getRead(offset);
    while (bam != NULL && offset < endOffset) {
        if (readIdsMatch(bam->read_name(), id)) {
            if (o_offset != NULL) {
                *o_offset = offset;
            }
            return bam;
        }
        bam = getNextRead(bam, &offset);
    }
    return NULL;
}

struct DuplicateReadKey
{
    DuplicateReadKey() { memset(this, 0, sizeof(DuplicateReadKey)); }

    DuplicateReadKey(const BAMAlignment* bam, const Genome* genome)
    {
        if (bam == NULL) {
            locations[0] = locations[1] = UINT32_MAX;
            isRC[0] = isRC[1] = false;
        } else {
            locations[0] = bam->getLocation(genome);
            locations[1] = bam->getNextLocation(genome);
            isRC[0] = (bam->FLAG & SAM_REVERSE_COMPLEMENT) != 0;
            isRC[1] = (bam->FLAG & SAM_NEXT_REVERSED) != 0;
            if (((((_uint64) locations[0]) << 1) | (isRC[0] ? 1 : 0)) > ((((_uint64) locations[1]) << 1) | (isRC[1] ? 1 : 0))) {
                const unsigned t = locations[1];
                locations[1] = locations[0];
                locations[0] = t;
                const bool f = isRC[1];
                isRC[1] = isRC[0];
                isRC[0] = f;
            }
        }
    }

    bool operator==(const DuplicateReadKey& b) const
    {
        return locations[0] == b.locations[0] && locations[1] == b.locations[1] &&
            isRC[0] == b.isRC[0] && isRC[1] == b.isRC[1];
    }

    bool operator!=(const DuplicateReadKey& b) const
    {
        return ! ((*this) == b);
    }

    bool operator<(const DuplicateReadKey& b) const
    {
        return locations[0] < b.locations[0] ||
            (locations[0] == b.locations[0] &&
                (locations[1] < b.locations[1] ||
                    (locations[1] == b.locations[1] &&
                        isRC[0] * 2 + isRC[1] <  b.isRC[0] *2 + b.isRC[1])));
    }


    // required for use as a key in VariableSizeMap template
    DuplicateReadKey(int x)
    { locations[0] = locations[1] = x; isRC[0] = isRC[1] = false; }
    bool operator==(int x) const
    { return locations[0] == (_uint32) x && locations[1] == (_uint32) x; }
    bool operator!=(int x) const
    { return locations[0] != (_uint32) x || locations[1] != (_uint32) x; }
    operator _uint64()
    { return ((_uint64) (locations[1] ^ (isRC[1] ? 1 : 0))) << 32 | (_uint64) (locations[0] ^ (isRC[0] ? 1 : 0)); }

    unsigned locations[2];
    bool isRC[2];
};

struct DuplicateMateInfo
{
    DuplicateMateInfo() { memset(this, 0, sizeof(DuplicateMateInfo)); }

    size_t firstRunOffset; // first read in duplicate set
    size_t firstRunEndOffset;
    size_t bestReadOffset[4]; // file offsets of first/second/new first/old second best reads
    int bestReadQuality[2]; // total quality of first/both best reads
    char bestReadId[120];

    void setBestReadId(const char* id) { strncpy(bestReadId, id, sizeof(bestReadId)); }
    const char* getBestReadId() { return bestReadId; }
};

class BAMDupMarkFilter : public BAMFilter
{
public:
    BAMDupMarkFilter(const Genome* i_genome) :
        BAMFilter(DataWriter::ModifyFilter),
        genome(i_genome), runOffset(0), runLocation(UINT32_MAX), runCount(0), mates()
    {}

    ~BAMDupMarkFilter()
    {
#ifdef USE_DEVTEAM_OPTIONS
        if (mates.size() > 0) {
            printf("duplicate matching ended with %d unmatched reads:\n", mates.size());
            for (MateMap::iterator i = mates.begin(); i != mates.end(); i = mates.next(i)) {
                printf("%u%s/%u%s\n", i->key.locations[0], i->key.isRC[0] ? "rc" : "", i->key.locations[1], i->key.isRC[1] ? "rc" : "");
            }
        }
#endif
    }

    static bool isDuplicate(const BAMAlignment* a, const BAMAlignment* b)
    { return a->pos == b->pos && a->refID == b->refID &&
        ((a->FLAG ^ b->FLAG) & (SAM_REVERSE_COMPLEMENT | SAM_NEXT_REVERSED)) == 0; }

protected:
    virtual void onRead(BAMAlignment* bam, size_t fileOffset, int batchIndex);

private:
    static int getTotalQuality(BAMAlignment* bam);

    const Genome* genome;
    size_t runOffset; // offset in file of first read in run
    _uint32 runLocation; // location in genome
    int runCount; // number of aligned reads
    typedef VariableSizeMap<DuplicateReadKey,DuplicateMateInfo,150,MapNumericHash<DuplicateReadKey>,70,0,-2> MateMap;
    static const _uint64 RunKey = 0xffffffffc0000000UL;
    static const _uint64 RunRC = 0x80000000;
    static const _uint64 RunNextRC = 0x40000000;
    static const _uint64 RunOffset = 0x3fffffff;
    typedef VariableSizeVector<_uint64> RunVector;
    RunVector run;
    MateMap mates;
};

    void
BAMDupMarkFilter::onRead(BAMAlignment* lastBam, size_t lastOffset, int)
{
    unsigned location = lastBam->getLocation(genome);
    unsigned nextLocation = lastBam->getNextLocation(genome);
    unsigned logicalLocation = location != UINT32_MAX ? location : nextLocation;
    if (logicalLocation == UINT32_MAX) {
        return;
    }
    if (logicalLocation == runLocation) {
        runCount++;
    } else {
        // if there was more than one read with same location, then analyze the run
        if (runCount > 1) {
            // partition by duplicate key, find best read in each partition
            size_t offset = runOffset;
            run.clear();
            // sort run by other coordinate & RC flags to get sub-runs
            for (BAMAlignment* record = getRead(offset); record != NULL && record != lastBam; record = getNextRead(record, &offset)) {
                // use opposite of logical location to sort records
                _uint64 entry = record->getLocation(genome) == UINT32_MAX
                    ? (((_uint64) UINT32_MAX) << 32) |
                        ((record->FLAG & SAM_REVERSE_COMPLEMENT) ? RunNextRC : 0) |
                        ((record->FLAG & SAM_NEXT_REVERSED) ? RunRC : 0)
                    : (((_uint64) record->getNextLocation(genome)) << 32) |
                        ((record->FLAG & SAM_REVERSE_COMPLEMENT) ? RunRC : 0) |
                        ((record->FLAG & SAM_NEXT_REVERSED) ? RunNextRC : 0);
                entry |= (_uint64) ((offset - runOffset) & RunOffset);
                _ASSERT(offset - runOffset <= RunOffset);
                run.push_back(entry);
            }
			if (run.size() == 0) {
				goto done; // todo: handle runs > n buffers (but should be rare!)
			}
            // ensure that adjacent half-mapped pairs stay together
            std::stable_sort(run.begin(), run.end());
            bool foundRun = false;
            for (RunVector::iterator i = run.begin(); i != run.end(); i++) {
                // skip singletons
                if ((i == run.begin() || (*i & RunKey) != (*(i-1) & RunKey)) &&
                    (i + 1 == run.end() || (*i & RunKey) != (*(i+1) & RunKey))) {
                    continue;
                }
                offset = runOffset + (*i & RunOffset);
                BAMAlignment* record = getRead(offset);
                _ASSERT(record->refID >= -1 && record->refID < genome->getNumContigs()); // simple sanity check
                // skip adjacent half-mapped pairs, they're not really runs
                if (i + 1 < run.end() && readIdsMatch(record->read_name(), getRead(runOffset + (*(i+1) & RunOffset))->read_name())) {
                    i++;
                    continue;
                }
                foundRun = true;
                DuplicateReadKey key(record, genome);
                MateMap::iterator f = mates.find(key);
                DuplicateMateInfo* info;
                if (f == mates.end()) {
                    mates.put(key, DuplicateMateInfo());
                    info = &mates[key];
                    //printf("add %u%s/%u%s -> %d\n", key.locations[0], key.isRC[0] ? "rc" : "", key.locations[1], key.isRC[1] ? "rc" : "", mates.size());
                    info->firstRunOffset = runOffset;
                    info->firstRunEndOffset = lastOffset;
                } else {
                    info = &f->value;
                }
                int totalQuality = getTotalQuality(record);
                size_t mateOffset = 0;
                BAMAlignment* mate = NULL;
                // optimize case for half-mapped pairs with adjacent reads
                mate = tryFindRead(info->firstRunOffset, info->firstRunEndOffset, record->read_name(), &mateOffset);
                if (mate == record) {
                    mate = NULL;
                }
                bool isSecond = mate != NULL;
                if (isSecond) {
                    totalQuality += getTotalQuality(mate);
                }
                if (totalQuality > info->bestReadQuality[isSecond]) {
                    info->bestReadQuality[isSecond] = totalQuality;
                    info->bestReadOffset[isSecond] = offset;
                    if (isSecond) {
                        info->bestReadOffset[2] = mateOffset;
                    }
                    info->setBestReadId(record->read_name());
                }
                if (isSecond && readIdsMatch(info->getBestReadId(), record->read_name())) {
                    info->bestReadOffset[3] = offset;
                }
            }
            if (! foundRun) {
                goto done; // avoid useless looping
            }
            // go back and adjust flags
            offset = runOffset;
            VariableSizeVector<DuplicateMateInfo*>* failedBackpatch = NULL;
            for (RunVector::iterator i = run.begin(); i != run.end(); i++) {
                // skip singletons
                if ((i == run.begin() || (*i & RunKey) != (*(i-1) & RunKey)) &&
                    (i + 1 == run.end() || (*i & RunKey) != (*(i+1) & RunKey))) {
                    continue;
                }
                offset = runOffset + (*i & RunOffset);
                BAMAlignment* record = getRead(offset);
                if (i + 1 < run.end() && readIdsMatch(record->read_name(), getRead(runOffset + (*(i+1) & RunOffset))->read_name())) {
                    i++;
                    continue;
                }
                DuplicateReadKey key(record, genome);
                MateMap::iterator m = mates.find(key);
                if (m == mates.end()) {
                    continue; // one end in a run, other not
                }
                DuplicateMateInfo* minfo = &m->value;
                bool pass = minfo->bestReadQuality[1] != 0; // 1 for second pass, 0 for first pass
                bool isSecond = minfo->firstRunOffset != runOffset;
                static const int index[2][2] = {{0, 3}, {2, 1}};
                if (offset != minfo->bestReadOffset[index[pass][isSecond]]) {
                    // Picard markDuplicates will not mark unmapped reads
                    if ((record->FLAG & SAM_UNMAPPED) == 0) {
                        record->FLAG |= SAM_DUPLICATE;
                    }
                } else if (pass == 1 && minfo->bestReadOffset[2] != 0 && minfo->bestReadOffset[0] != 0 && minfo->bestReadOffset[2] != minfo->bestReadOffset[0]) {
                    // backpatch reads in first matelist if they're still in memory
                    BAMAlignment* oldBest = getRead(minfo->bestReadOffset[0]);
                    BAMAlignment* newBest = getRead(minfo->bestReadOffset[2]);
                    if (oldBest != NULL && newBest != NULL) {
                        oldBest->FLAG &= ~SAM_DUPLICATE;
                        newBest->FLAG |= SAM_DUPLICATE;
                    } else {
                        if (failedBackpatch == NULL) {
                            failedBackpatch = new VariableSizeVector<DuplicateMateInfo*>();
                        }
                        failedBackpatch->push_back(minfo);
                    }
                }
            }

            // fixup any that failed
            if (failedBackpatch != NULL) {
                for (VariableSizeVector<DuplicateMateInfo*>::iterator i = failedBackpatch->begin(); i != failedBackpatch->end(); i++) {
                    // couldn't go back and patch first set to have correct best for second set
                    // so patch second set to have same best as first set even though it's not really the best
                    BAMAlignment* trueBestSecond = getRead((*i)->bestReadOffset[1]);
                    BAMAlignment* firstBestSecond = getRead((*i)->bestReadOffset[3]);
                    _ASSERT(trueBestSecond != NULL && firstBestSecond != NULL);
                    if (trueBestSecond != NULL && firstBestSecond != NULL) {
                        trueBestSecond->FLAG &= ~SAM_DUPLICATE;
                        firstBestSecond->FLAG |= ~SAM_DUPLICATE;
                    }
                }
            }

            // clean up
            for (RunVector::iterator i = run.begin(); i != run.end(); i++) {
                // skip singletons
                if ((i == run.begin() || (*i & RunKey) != (*(i-1) & RunKey)) &&
                    (i + 1 == run.end() || (*i & RunKey) != (*(i+1) & RunKey))) {
                    continue;
                }
                offset = runOffset + (*i & RunOffset);
                BAMAlignment* record = getRead(offset);
                if (i + 1 < run.end() && readIdsMatch(record->read_name(), getRead(runOffset + (*(i+1) & RunOffset))->read_name())) {
                    i++;
                    continue;
                }
                DuplicateReadKey key(record, genome);
                MateMap::iterator m = mates.find(key);
                if (m != mates.end() && m->value.firstRunOffset != runOffset) {
                    mates.erase(key);
                    //printf("erase %u%s/%u%s -> %d\n", key.locations[0], key.isRC[0] ? "rc" : "", key.locations[1], key.isRC[1] ? "rc" : "", mates.size());
                }
            }
        }
done:
        runLocation = logicalLocation;
        runOffset = lastOffset;
        runCount = 1;
    }
    // todo: preserve this across batches - need to block-copy entire memory for reads
}

    int
BAMDupMarkFilter::getTotalQuality(
    BAMAlignment* bam)
{
    int result = 0;
    _uint8* p = (_uint8*) bam->qual();
    for (int i = 0; i < bam->l_seq; i++) {
        int q = *p++;
        result += (q != 255) * q; // avoid branch?
    }
    return result;
}

class BAMDupMarkSupplier : public DataWriter::FilterSupplier
{
public:
    BAMDupMarkSupplier(const Genome* i_genome) :
        FilterSupplier(DataWriter::ReadFilter), genome(i_genome) {}

    virtual DataWriter::Filter* getFilter()
    { return new BAMDupMarkFilter(genome); }

    virtual void onClosing(DataWriterSupplier* supplier) {}
    virtual void onClosed(DataWriterSupplier* supplier) {}

private:
    const Genome* genome;
};

    DataWriter::FilterSupplier*
DataWriterSupplier::markDuplicates(const Genome* genome)
{
    return new BAMDupMarkSupplier(genome);
}

class BAMIndexSupplier;

class BAMIndexFilter : public BAMFilter
{
public:
    BAMIndexFilter(BAMIndexSupplier* i_supplier)
        : BAMFilter(DataWriter::ReadFilter), supplier(i_supplier) {}

protected:
    virtual void onRead(BAMAlignment* bam, size_t fileOffset, int batchIndex);

private:
    BAMIndexSupplier* supplier;
};

class BAMIndexSupplier : public DataWriter::FilterSupplier
{
public:
    BAMIndexSupplier(const char* i_indexFileName, const Genome* i_genome, GzipWriterFilterSupplier* i_gzipSupplier) :
        FilterSupplier(DataWriter::ReadFilter),
        indexFileName(i_indexFileName),
        genome(i_genome),
        gzipSupplier(i_gzipSupplier),
        lastRefId(-1),
        lastBin(0), binStart(0), lastBamEnd(0)
    {
        refs = genome ? new RefInfo[genome->getNumContigs()] : NULL;
        readCounts[0] = readCounts[1] = 0;
    }

    virtual DataWriter::Filter* getFilter()
    { return new BAMIndexFilter(this); }

    virtual void onClosing(DataWriterSupplier* supplier) {}
    virtual void onClosed(DataWriterSupplier* supplier);

private:

    friend class BAMIndexFilter;

    struct BAMChunk {
        BAMChunk() : start(0), end(0) {}
        BAMChunk(const BAMChunk& a) : start(a.start), end(a.end) {}

        _uint64 start, end;
    };
    typedef VariableSizeVector<BAMChunk> ChunkVec;
    typedef VariableSizeMap<_uint32,ChunkVec,150,MapNumericHash<_uint32>,80,-1,-2> BinMap;
    typedef VariableSizeVector<_uint64> LinearMap;
    struct RefInfo {
        BinMap bins;
        LinearMap intervals;
    };

    RefInfo* getRefInfo(int refId);

    void onRead(BAMAlignment* bam, size_t fileOffset, int batchIndex);

    void addChunk(int refId, _uint32 bin, _uint64 start, _uint64 end);

    void addInterval(int refId, int begin, int end, _uint64 fileOffset);

    const char* indexFileName;
    const Genome* genome;
    int lastRefId;
    _uint32 lastBin;
    _uint64 binStart;
    _uint64 firstBamStart;
    _uint64 lastBamEnd;
    _uint64 readCounts[2]; // mapped, unmapped
    RefInfo* refs;
    GzipWriterFilterSupplier* gzipSupplier;
};

    void
BAMIndexFilter::onRead(
    BAMAlignment* bam,
    size_t fileOffset,
    int batchIndex)
{
    supplier->onRead(bam, fileOffset, batchIndex);
}

    DataWriter::FilterSupplier*
DataWriterSupplier::bamIndex(
    const char* indexFileName,
    const Genome* genome,
    GzipWriterFilterSupplier* gzipSupplier)
{
    return new BAMIndexSupplier(indexFileName, genome, gzipSupplier);
}

    void
BAMIndexSupplier::onRead(
    BAMAlignment* bam,
    size_t fileOffset,
    int batchIndex)
{
    //printf("index onRead %d:%d+%d @ %lld %d\n", bam->refID, bam->pos, bam->l_ref(), fileOffset, batchIndex);
    if (bam->refID != lastRefId) {
        if (lastRefId != -1) {
            addChunk(lastRefId, BAMAlignment::BAM_EXTRA_BIN, firstBamStart, lastBamEnd);
            addChunk(lastRefId, BAMAlignment::BAM_EXTRA_BIN, readCounts[0], readCounts[1]);
            readCounts[0] = readCounts[1] = 0;
        }
        firstBamStart = fileOffset;
    }
    readCounts[(bam->FLAG & SAM_UNMAPPED) ? 1 : 0]++;
    if (bam->refID != lastRefId || bam->bin != lastBin || lastRefId == -1) {
        addChunk(lastRefId, lastBin, binStart, fileOffset);
        lastBin = bam->bin;
        lastRefId = bam->refID;
        binStart = fileOffset;
    }
    if (! (bam->FLAG & SAM_UNMAPPED)) {
        _ASSERT(bam->pos != -1 && bam->refID != -1);
        addInterval(bam->refID, bam->pos, bam->pos + bam->l_ref() - 1, fileOffset);
    }
    lastBamEnd = fileOffset + bam->size();
}

    void
BAMIndexSupplier::onClosed(
    DataWriterSupplier* supplier)
{
    // add final chunk
    if (lastRefId != -1) {
        addChunk(lastRefId, lastBin, binStart, lastBamEnd);
        addChunk(lastRefId, BAMAlignment::BAM_EXTRA_BIN, firstBamStart, lastBamEnd);
        addChunk(lastRefId, BAMAlignment::BAM_EXTRA_BIN, readCounts[0], readCounts[1]);
    }

    // write out index file
    FILE* index = fopen(indexFileName, "wb");
    char magic[4] = {'B', 'A', 'I', 1};
    fwrite(magic, sizeof(magic), 1, index);
    _int32 n_ref = genome->getNumContigs();
    fwrite(&n_ref, sizeof(n_ref), 1, index);

    for (int i = 0; i < n_ref; i++) {
        RefInfo* info = getRefInfo(i);
        _int32 n_bin, n_intv;
        if (info == NULL) {
            n_bin = 0;
            fwrite(&n_bin, sizeof(n_bin), 1, index);
            n_intv = 0;
            fwrite(&n_intv, sizeof(n_intv), 1, index);
            continue;
        }
        n_bin = info->bins.size();
        fwrite(&n_bin, sizeof(n_bin), 1, index);
        for (BinMap::iterator j = info->bins.begin(); j != info->bins.end(); j = info->bins.next(j)) {
            _uint32 bin = j->key;
            fwrite(&bin, sizeof(bin), 1, index);
            _int32 n_chunk = j->value.size();
            fwrite(&n_chunk, sizeof(n_chunk), 1, index);
            if (bin != BAMAlignment::BAM_EXTRA_BIN) {
                for (ChunkVec::iterator k = j->value.begin(); k != j->value.end(); k++) {
                    _uint64 chunk[2] = {gzipSupplier->toVirtualOffset(k->start), gzipSupplier->toVirtualOffset(k->end)};
                    fwrite(&chunk, sizeof(chunk), 1, index);
                }
            } else {
                _uint64 chunk[2] = {gzipSupplier->toVirtualOffset(j->value[0].start), gzipSupplier->toVirtualOffset(j->value[0].end)};
                fwrite(&chunk, sizeof(chunk), 1, index);
                chunk[0] = j->value[1].start;
                chunk[1] = j->value[1].end;
                fwrite(&chunk, sizeof(chunk), 1, index);
            }
        }
        n_intv = info->intervals.size();
        fwrite(&n_intv, sizeof(n_intv), 1, index);
        for (LinearMap::iterator m = info->intervals.begin(); m != info->intervals.end(); m++) {
            _uint64 ioffset = gzipSupplier->toVirtualOffset(*m);
            fwrite(&ioffset, sizeof(ioffset), 1, index);
        }
    }
    fclose(index);
}

   BAMIndexSupplier::RefInfo*
BAMIndexSupplier::getRefInfo(
    int refId)
{
    return refId >= 0 && refId < genome->getNumContigs() ? &refs[refId] : NULL;
}

    void
BAMIndexSupplier::addChunk(
    int refId,
    _uint32 bin,
    _uint64 start,
    _uint64 end)
{
    RefInfo* info = getRefInfo(refId);
    if (info == NULL) {
        return;
    }
    ChunkVec* chunks = info->bins.tryFind(bin);
    if (chunks == NULL) {
        ChunkVec empty;
        info->bins.tryAdd(bin, empty, &chunks);
    }
    BAMChunk chunk;
    chunk.start = start;
    chunk.end = end;
    chunks->push_back(chunk);
}

    void
BAMIndexSupplier::addInterval(
    int refId,
    int begin,
    int end,
    _uint64 fileOffset)
{
    RefInfo* info = getRefInfo(refId);
    if (info == NULL) {
        return;
    }
    int slot = end <= 0 ? 0 : ((end - 1) / 16384);
    if (slot >= info->intervals.size()) {
        for (int i = info->intervals.size(); i < slot; i++) {
            info->intervals.push_back(UINT64_MAX);
        }
        info->intervals.push_back(fileOffset);
    }
}

    bool
BgzfHeader::validate(char* buffer, size_t bytes)
{
    char* p;
    for (p = buffer; p - buffer < (_int64)bytes; ) {
        BgzfHeader* h = (BgzfHeader*) p;
        unsigned bsize = h->BSIZE() + 1;
        unsigned isize = h->ISIZE();
        if (bsize == 0 || bsize > BAM_BLOCK || isize > BAM_BLOCK ||
                bsize > max(2 * isize, isize+1000) || ! h->validate(bsize, isize)) {
            return false;
        }
        p += bsize;
    }
    return p == buffer + bytes;
}

    bool
BgzfHeader::validate(
    size_t compressed,
    size_t uncompressed)
{
    return ID1 == 0x1f && ID2 == 0x8b && CM == 8 && FLG == 4 &&
        MTIME == 0 && XFL == 0 && OS == 0 &&
        ISIZE() == uncompressed&&
        BSIZE() + 1 == compressed;
}
