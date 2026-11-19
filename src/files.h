/* files: read and write on paths, the two tools that run inside the kernel and finish at
   once (protocols.md §5): a text file by line range, any file as an image or document block,
   and atomic replace or append. Each produces a tool result's content and whether it failed.
   Which tool a call is, and writing to a job's stdin, are calls'. Error text states what is
   wrong, never what to do about it. */

#ifndef FILES_H
#define FILES_H

#include "json.h"

bool tool_read(json input, json_buf *result);
bool tool_write_file(json input, json_buf *result);

#endif
