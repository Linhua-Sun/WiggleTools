// Copyright (c) 2013, Daniel Zerbino
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// 
// (1) Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer. 
// 
// (2) Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in
// the documentation and/or other materials provided with the
// distribution.  
// 
// (3)The name of the author may not be used to
// endorse or promote products derived from this software without
// specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
// IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <stdlib.h>
#include <math.h>
#include <gsl/gsl_cdf.h>

#include "multiSet.h"

typedef struct setComparisonData_st {
	Multiset * multi;
} SetComparisonData;

void SetComparisonSeek(WiggleIterator * iter, const char * chrom, int start, int finish) {
	SetComparisonData * data = (SetComparisonData* ) iter->data;
	seekMultiset(data->multi, chrom, start, finish);
	pop(iter);
}

////////////////////////////////////////////////////////
// T-test
////////////////////////////////////////////////////////

bool TTestReductionPop2(WiggleIterator * wi) {
	if (wi->done)
		return true;

	SetComparisonData * data = (SetComparisonData *) wi->data;
	Multiset * multi = data->multi;

	if (multi->done) {
		wi->done = true;
		return true;
	}

	// Go to first position where both of the sets have at least one value
	while (!multi->inplay[0] || !multi->inplay[1]) {
		popMultiset(multi);
		if (multi->done) {
			wi->done = true;
			return true;
		}
	}
	wi->chrom = multi->chrom;
	wi->start = multi->start;
	wi->finish = multi->finish;

	// Compute measurements
	double sum1, sum2, sumSq1, sumSq2;
	int count1, count2;
	int index;

	sum1 = sum2 = 0;
	sumSq1 = sumSq2 = 0;
	count1 = multi->multis[0]->count;
	count2 = multi->multis[1]->count;

	for (index = 0; index < multi->multis[0]->count; index++) {
		if (multi->multis[0]->inplay[index]) {
			sum1 += multi->values[0][index];
			sumSq1 += multi->values[0][index] * multi->values[0][index];
		}
	}

	for (index = 0; index < multi->multis[1]->count; index++) {
		if (multi->multis[1]->inplay[index]) {
			sum2 += multi->values[1][index];
			sumSq2 += multi->values[1][index] * multi->values[1][index];
		}
	}

	// To avoid divisions by 0:
	if (count1 == 0 || count2 == 0) {
		popMultiset(multi);
		pop(wi);	
		return;
	}

	double mean1 = sum1 / count1;
	double mean2 = sum2 / count2;
	double meanSq1 = sumSq1 / count1;
	double meanSq2 = sumSq2 / count2;
	double var1 = meanSq1 - mean1 * mean1;
	double var2 = meanSq2 - mean2 * mean2;

	// To avoid divisions by 0:
	if (var1 + var2 == 0) {
		popMultiset(multi);
		return false;
	}

	// T-statistic

	double t = (mean1 - mean2) / sqrt(var1 / count1 + var2 / count2);

	if (t < 0)
		t = -t;

	// Degrees of freedom
	
	double nu = (var1 / count1 + var2 / count2) * (var1 / count1 + var2 / count2) / ((var1 * var1) / (count1 * count1 * (count1 - 1)) + (var2 * var2) / (count2 * count2 * (count2 - 1)));

	// P-value

	wi->value = 2 * gsl_cdf_tdist_Q(t, nu);

	// Update inputs
	popMultiset(multi);
	return true;
}

void TTestReductionPop(WiggleIterator * wi) {
	while (true)
		if (wi->done || TTestReductionPop2(wi))
			break;
}

WiggleIterator * TTestReduction(Multiset * multi) {
	SetComparisonData * data = (SetComparisonData *) calloc(1, sizeof(SetComparisonData));
	if (multi->count != 2 || multi->multis[0]->count + multi->multis[1]->count < 3) {
		puts("The t-test function only works for two sets with enough elements to compute variance");
		exit(1);
	}	
	data->multi = multi;
	return newWiggleIterator(data, &TTestReductionPop, &SetComparisonSeek);
}

////////////////////////////////////////////////////////
// Mann-Whitney U (Wilcoxon rank-sum test)
////////////////////////////////////////////////////////

typedef struct valueSetPair_st {
	double value;
	bool set;
} ValueSetPair;

typedef struct mwuData_st {
	Multiset * multi;
	int n1;
	int n2;
	int N;
	// Pre-allocated table for sorting
	ValueSetPair * rankingTable;
	// For normal approximation
	bool normalApproximation;
	double mu_U, sigma_U;
} MWUData;

void MWUSeek(WiggleIterator * iter, const char * chrom, int start, int finish) {
	MWUData * data = (MWUData* ) iter->data;
	seekMultiset(data->multi, chrom, start, finish);
	pop(iter);
}

static int compareValueSetPairs(const void * A, const void * B) {
	ValueSetPair * vspA = (ValueSetPair *) A;
	ValueSetPair * vspB = (ValueSetPair *) B;
	if (vspA->value < vspB->value)
		return -1;
	if (vspA->value > vspB->value)
		return 1;
	return 0;
}

bool MWUReductionPop2(WiggleIterator * wi) {
	if (wi->done)
		return true;

	MWUData * data = (MWUData *) wi->data;
	Multiset * multi = data->multi;

	if (multi->done) {
		wi->done = true;
		return true;
	}

	// Go to first position where both of the sets have at least one value
	while (!multi->inplay[0] || !multi->inplay[1]) {
		popMultiset(multi);
		if (multi->done) {
			wi->done = true;
			return true;
		}
	}
	wi->chrom = multi->chrom;
	wi->start = multi->start;
	wi->finish = multi->finish;

	// Compute measurements
	int index;
	ValueSetPair * vspPtr = data->rankingTable;

	for (index = 0; index < data->n1; index++) {
		if (multi->multis[0]->inplay[index]) 
			vspPtr->value = multi->values[0][index];
		else
			vspPtr->value = 0;
		vspPtr->set = false;
		vspPtr++;
	}

	for (index = 0; index < data->n2; index++) {
		if (multi->multis[1]->inplay[index]) 
			vspPtr->value = multi->values[1][index];
		else
			vspPtr->value = 0;
		vspPtr->set = true;
		vspPtr++;
	}

	qsort(data->rankingTable, data->N, sizeof(ValueSetPair), compareValueSetPairs);

	// Sum of ranks of elements of set 1
	double U1 = 0;
	// Rolling count of elements of set 1 seen prior on the list
	int prev = 0;
	// Warns you when you have a tie with the previously visited Value-Set pairs
	int ties = 0;
	int previousTies = 0;
	vspPtr = data->rankingTable;
	for (index = 0; index < data->N && prev < data->n1; index++) {
		if (!vspPtr->set) {
			U1 += index - prev;
			if (ties) {
				// Look for ties on the table prior to the current position and after the last occurence of an element of set 1.
				int index2;
				for (index2 = index + 1; index2 < data->N && data->rankingTable[index2].value == vspPtr->value && data->rankingTable[index2].set; index2++)
					previousTies++;
				U1 -= previousTies / 2.0;
				U1 += (ties - previousTies) / 2.0;
				if (previousTies == ties)
					previousTies = ties = 0;
			} else {
				int index2;
				// Look for ties with next values
				for (index2 = index + 1; index2 < data->N && data->rankingTable[index2].value == vspPtr->value; index2++)
					if (data->rankingTable[index2].set)
						ties++;
				if (ties) 
					U1 += ties / 2.0;
			}
			prev++;
		}
		vspPtr++;
	}

	if (data->normalApproximation) {
		if (U1 > data->mu_U)
			wi->value = 2 * erf((data->mu_U - U1) / data->sigma_U);
		else
			wi->value = 2 * erf((U1 - data->mu_U) / data->sigma_U);
	}

	// Update inputs
	popMultiset(multi);
	return true;
}


void MWUReductionPop(WiggleIterator * wi) {
	while (true)
		if (wi->done || MWUReductionPop2(wi))
			break;
}

WiggleIterator * MWUReduction(Multiset * multi) {
	MWUData * data = (MWUData *) calloc(1, sizeof(MWUData));
	if (multi->count != 2 || multi->multis[0]->count == 0 || multi->multis[1]->count == 0) {
		puts("The Mann-Whitney U function only works for two non-empty sets");
		exit(1);
	}	
	data->multi = multi;
	data->n1 = multi->multis[0]->count;
	data->n2 = multi->multis[1]->count;
	data->N = data->n1 + data->n2;
	data->rankingTable = calloc(data->N, sizeof(ValueSetPair));
	if (true) {
		// Ideally, tables could be used for small values of n1 and n2
		data->normalApproximation = true;
		data->mu_U = data->n1 * data->n2 / 2;
		data->sigma_U = sqrt(data->n1 * data->n2 * (data->n1 + data->n2 + 1) / 12);
	}
	return newWiggleIterator(data, &MWUReductionPop, &MWUSeek);
}