# Local hardening for the hash-pinned quirc 1.2 source. Fail configure if an upstream
# update changes these sites, so a version bump cannot silently drop the guards.
# References: https://github.com/dlbeer/quirc/issues/162 and /pull/159.
function(quirc_replace old new)
    string(FIND "${source}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "quirc hardening site changed; review the upstream update")
    endif()
    string(REPLACE "${old}" "${new}" source "${source}")
    set(source "${source}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/quirc)
file(READ ${quirc_SOURCE_DIR}/lib/identify.c source)
quirc_replace("int size_estimate;" "double size_estimate;")
quirc_replace([=[size_estimate = abs((a.x - b.x) * -(c.y - b.y) +
			    (a.y - b.y) * (c.x - b.x));]=]
    [=[size_estimate = fabs(((double)a.x - b.x) * -((double)c.y - b.y) +
                            ((double)a.y - b.y) * ((double)c.x - b.x));
    if (!isfinite(size_estimate) || size_estimate > (double)q->w * q->h ||
        b.x < 0 || b.x >= q->w || b.y < 0 || b.y >= q->h)
        return;]=])
quirc_replace("while (step_size * step_size < size_estimate * 100) {"
    "while (step_size <= 256 && step_size <= q->w && step_size <= q->h && (double)step_size * step_size < size_estimate * 100) {")
quirc_replace("measure_grid_size(q, qr_index);"
    "measure_grid_size(q, qr_index);\n\tif (qr->grid_size < 21 || qr->grid_size > 177) goto fail;")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/quirc/identify.c "${source}")
file(READ ${quirc_SOURCE_DIR}/lib/quirc.c source)
quirc_replace("(void)memcpy(image, q->image, min);" "if (min > 0) (void)memcpy(image, q->image, min);")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/quirc/quirc.c "${source}")
