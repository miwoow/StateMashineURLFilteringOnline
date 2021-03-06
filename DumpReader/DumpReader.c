/*
 * DumpReader.c
 *
 *  Created on: Jan 17, 2011
 *      Author: yotamhc
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include "DumpReader.h"
#include "BoundedBuffer/Scanner.h"
#include "BoundedBuffer/PacketReader.h"
#include "../StateMachine/TableStateMachine.h"
#include "../Dedup/DictionaryReader.h"

#ifdef PAPI
#include <papi.h>
#endif

#define BYTES_TO_MBITS 8.0/(1024 * 1024)
#define GET_TRANSFER_RATE(bytes, timer) (((bytes) * BYTES_TO_MBITS) / ((timer.micros) / 1000000.0))


void startTiming(Timer *timer) {
	gettimeofday(&(timer->start), NULL);
}

void endTiming(Timer *timer) {
	gettimeofday(&(timer->finish), NULL);
	timer->micros += (timer->finish.tv_sec * 1000000 + timer->finish.tv_usec) - (timer->start.tv_sec * 1000000 + timer->start.tv_usec);
}

#ifdef PAPI
#define NUM_PAPI_EVENTS 3
#endif

void inspectDumpFile(const char *path, const char *dict_path, int dict_width, StateMachine *machine, int isTableMachine, int verbose, int timing, int threads) {
	double rate, rateWithHeaders;
	Timer t;
	long size, sizeWithHeaders;
	int i, j, count;
	Dictionary *dict;
	RollingHashTool hasher;

#ifdef PAPI
	int papi_events[] = {
			PAPI_L1_DCR,
			PAPI_L2_DCR,
			PAPI_L3_DCR,
	};
	char *papi_event_names[] = {
			"PAPI_L1_DCR",
			"PAPI_L2_DCR",
			"PAPI_L3_DCR",
	};
	long long papi_values[NUM_PAPI_EVENTS];
	int papi_status;
#endif

	ScannerData *scanners;
	PacketReaderData reader;
	LinkedList *packet_queues;

#ifdef COUNT_FAIL_PERCENT
	long totalFailures, totalGotos;
#endif

	if (dict_path && (!isTableMachine)) {
		fprintf(stderr, "Error: cannot use dictionary with compressed state machine.");
		exit(1);
	}
	init_hash(&hasher);

	packet_queues = (LinkedList*)malloc(sizeof(LinkedList) * threads);
	scanners = (ScannerData*)malloc(sizeof(ScannerData) * threads);
	count = 0;

	for (i = 0; i < threads; i++) {
		list_init(&packet_queues[i]);
	}

	packetreader_init(&reader, path, packet_queues, threads);
	for (i = 0; i < threads; i++) {
		if (dict_path) {
			dict = (Dictionary*)malloc(sizeof(Dictionary));
			dictionary_init(dict, 0, dict_width);
			dictionaryreader_read(dict_path, dict, &hasher, (TableStateMachine*)machine);
			for (j = 0; j < dict->size; j++) {
				if (dict->table[j])
					count++;
			}
			printf("Scanner %d dictionary contains %d entries\n", i, count);
		} else {
			dict = NULL;
		}
		scanner_init(&(scanners[i]), machine, isTableMachine, &packet_queues[i], dict, &hasher, dict_width, verbose);
	}

	packetreader_start(&reader);

	packetreader_join(&reader);

#ifdef PAPI
	/******** PAPI START ********/
	papi_status = PAPI_start_counters(papi_events, NUM_PAPI_EVENTS);
	/********* PAPI END *********/
#endif

	if (timing) {
		t.micros = 0;
		startTiming(&t);
	}

	for (i = 0; i < threads; i++) {
		scanner_start(&(scanners[i]));
	}

	for (i = 0; i < threads; i++) {
		scanner_join(&(scanners[i]));
	}

	if (timing) {
		endTiming(&t);
	}

#ifdef PAPI
	/******** PAPI START ********/
	if (papi_status == PAPI_OK) {
		if (PAPI_stop_counters(papi_values, NUM_PAPI_EVENTS) == PAPI_OK) {
			printf("PAPI Results:\n");
			for (i = 0; i < NUM_PAPI_EVENTS; i++) {
				printf("\t%s: %lld\n", papi_event_names[i], papi_values[i]);
			}
		}
	} else {
		fprintf(stderr, "PAPI Error %d\n", papi_status);
	}
	/********* PAPI END *********/
#endif

	if (timing) {
		size = reader.size;
		sizeWithHeaders = reader.sizeWithHeaders;
		rate = GET_TRANSFER_RATE(size, t);
		rateWithHeaders = GET_TRANSFER_RATE(sizeWithHeaders, t);

		printf("Time(micros)\tData(No H)\tData(w/ H)\tRate(No H) Mb/s\tRate (w/ H) Mb/s\n");
		printf("%8ld\t%9ld\t%9ld\t%5.4f\t%5.4f\n", t.micros, size, sizeWithHeaders, rate, rateWithHeaders);
	}

	printf("\n");
	for (i = 0; i < threads; i++) {
#ifdef COUNT_DICTIONARY_SKIPPED_BYTES
		printf("Scanner %d skipped %ld out of %ld bytes (%3.3f%%)\n", i, scanners[i].totalSkipped, scanners[i].totalBytes,
				100 * ((double)scanners[i].totalSkipped) / scanners[i].totalBytes);
		printf("Scanner %d average number of chars scaned on left borders: %3.3f\n", i, ((double)(scanners[i].totalLeftBorderChars) / scanners[i].totalLeftBorders));
#endif
#ifdef COUNT_MEMCMP_FAILURES
		printf("Scanner %d dictionary queries: %ld, BF positives: %ld, BF false-pos: %ld (BF-FPR: %3.3f%%, BF-TPR: %3.3f%%)\n",
				i, scanners[i].totalDictGetTries, scanners[i].totalMemcmpTries, scanners[i].totalMemcmpFails,
				100 * ((double)scanners[i].totalMemcmpFails) / scanners[i].totalDictGetTries,
				100 * ((double)(scanners[i].totalMemcmpTries - scanners[i].totalMemcmpFails)) / scanners[i].totalDictGetTries
				);
#endif
		scanner_free_packets(&(scanners[i]));
		if (scanners[i].dict) {
			dictionary_destroy(scanners[i].dict);
			free(scanners[i].dict);
			scanners[i].dict = NULL;
		}
	}

#ifdef COUNT_FAIL_PERCENT
	totalFailures = totalGotos = 0;
        for (i = 0; i < threads; i++) {
                totalFailures += scanners[i].stats.totalFailures;
		totalGotos += scanners[i].stats.totalGotos;
        }

        printf("Fail percent: %f\n", ((double)totalFailures) / (totalFailures + totalGotos));
        printf("Total failures: %ld, Total gotos: %ld\n", totalFailures, totalGotos);
#endif

}

#define TEST_INPUT "/Users/yotamhc/Documents/workspace/AC-NFA-C/victor/data.bin"

void runTest(StateMachine *machine, int isTableMachine) {
	FILE *file;
	int buffSize, len;
	char *buff;
	double rate;
	Timer t;
	MachineStats stats;

	stats.totalFailures = 0;
	stats.totalGotos = 0;

	file = fopen(TEST_INPUT, "rb");
	if (!file) {
		fprintf(stderr, "Error opening file for reading\n");
		exit(1);
	}

	fseek(file, 0L, SEEK_END);
	buffSize = ftell(file);
	fseek(file, 0L, SEEK_SET);

	buff = (char*)malloc(sizeof(char) * buffSize);
	if (buff == NULL) {
		fprintf(stderr, "Error allocating memory for buffer\n");
		exit(1);
	}
	len = fread(buff, sizeof(char), buffSize, file);
	if (len != buffSize) {
		fprintf(stderr, "Error reading data from file\n");
		exit(1);
	}

	t.micros = 0;
	if (isTableMachine) {
		startTiming(&t);
		matchTableMachine((TableStateMachine*)machine, buff, buffSize, 1, NULL, NULL, NULL, NULL);
		endTiming(&t);
	} else {
		startTiming(&t);
		match(machine, buff, buffSize, 0, &stats);
		endTiming(&t);
	}
	rate = GET_TRANSFER_RATE(buffSize, t);

	printf("Time(micros)\tData(No H)\tData(w/ H)\tRate(No H) Mb/s\tRate (w/ H) Mb/s\n");
	printf("%8ld\t%9d\t%9d\t%5.4f\t%5.4f\n", t.micros, buffSize, buffSize, rate, rate);

	free(buff);

	fclose(file);
}


