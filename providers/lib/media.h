/* media: holding the media blocks of Messages-shaped messages to an API's size limits, the ones
   whose errors name no block. Owns finding the base64 images and documents (also inside tool and
   job results), judging their sizes against an API's limits (bytes per image, bytes per request,
   oldest dropped first) and writing the messages with each dropped block replaced by a note.
   Knows no API and no format: each provider brings its own limits. */

#ifndef MEDIA_H
#define MEDIA_H

#include "json.h"

/* Sizes are in base64 bytes, as the request carries them; 0 is no limit. */
typedef struct {
    size_t image_bytes, budget_bytes, document_budget_bytes;
} media_limits;

void media_put_within_limits(json_buf *out, json messages, const media_limits *limits);

#endif
