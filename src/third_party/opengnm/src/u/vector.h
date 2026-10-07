#ifndef _U_VECTOR_H_
#define _U_VECTOR_H_

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	void* data;
	size_t numelements;
	size_t maxelements;
	size_t elementsize;
} UVec;

static inline UVec uvalloc(size_t elemsize, size_t numelems) {
	assert(elemsize > 0);

	void* data = 0;
	if (numelems > 0) {
		const size_t newsize = numelems * elemsize;
		data = malloc(newsize);
		assert(data);
		memset(data, 0, newsize);
	}

	return (UVec){
	    .data = data,
	    .numelements = numelems,
	    .maxelements = numelems,
	    .elementsize = elemsize,
	};
}

static inline void uvclear(UVec* vec) {
	vec->numelements = 0;
}

static inline void uvfree(UVec* vec) {
	if (vec->data) {
		free(vec->data);
		vec->data = 0;
	}

	vec->numelements = 0;
	vec->maxelements = 0;
}

static inline UVec uvcopy(const UVec* other) {
	UVec vec = *other;

	if (other->data) {
		const size_t newsize = vec.numelements * vec.elementsize;
		vec.data = malloc(newsize);
		assert(vec.data);
		memcpy(vec.data, other->data, newsize);
	}

	return vec;
}

static inline void uvreserve(UVec* vec, size_t reservecount) {
	if (vec->maxelements >= reservecount) {
		return;
	}

	const size_t oldsize = vec->maxelements * vec->elementsize;
	const size_t newsize = reservecount * vec->elementsize;

	vec->data = realloc(vec->data, newsize);
	assert(vec->data);
	memset((char*)vec->data + oldsize, 0, newsize - oldsize);

	vec->maxelements = reservecount;
}

static inline void uvappendmany(
    UVec* vec, const void* element, size_t numelems
) {
	assert(vec->elementsize > 0);
	assert(numelems > 0);

	const size_t newnumelems = vec->numelements + numelems;
	if (newnumelems > vec->maxelements) {
		uvreserve(vec, newnumelems * 1.5);
	}

	void* nextdata =
	    (char*)vec->data + (vec->numelements * vec->elementsize);
	memcpy(nextdata, element, vec->elementsize * numelems);
	vec->numelements += numelems;
}

static inline void uvappend(UVec* vec, const void* element) {
	uvappendmany(vec, element, 1);
}

static inline void* uvdata(UVec* vec, size_t index) {
	assert(vec->data);
	return (char*)vec->data + (index * vec->elementsize);
}

static inline const void* uvdatac(const UVec* vec, size_t index) {
	assert(vec->data);
	return (const char*)vec->data + (index * vec->elementsize);
}

static inline size_t uvlen(const UVec* vec) {
	return vec->numelements;
}

static inline size_t uvbytelen(const UVec* vec) {
	return vec->numelements * vec->elementsize;
}

#endif	// _U_VECTOR_H_
