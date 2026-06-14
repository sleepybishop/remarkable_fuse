#ifndef PDFOVERLAY_H
#define PDFOVERLAY_H

/*
 * Overlays a PNG image onto a specific page of an existing PDF file.
 *
 * Parameters:
 *   pdf_path     - Path to the input PDF file.
 *   png_path     - Path to the PNG image to overlay.
 *   out_pdf_path - Path where the output PDF should be saved.
 *   page_num     - 1-based page number to overlay on.
 *   x, y         - Bottom-left position of the image in PDF points.
 *   width, height- Target dimensions in PDF points. If <= 0, auto-scaled.
 *
 * Returns:
 *   0 on success, non-zero on error.
 */
int pdf_overlay_png(const char *pdf_path, const char *png_path,
                    const char *out_pdf_path, int page_num, double x, double y,
                    double width, double height, int margins);

#endif /* PDFOVERLAY_H */
