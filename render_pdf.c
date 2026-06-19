#include "render_pdf.h"
#include "template_renderer.h"
#include <math.h>

void remfmt_render_pdf(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm) {
  float min_x = 0.0f;
  float max_x = (float)DEV_W;
  float min_y = 0.0f;
  float max_y = (float)DEV_H;

  if (strokes != NULL && !(prm && prm->annotation)) {
    for (int i = 0; i < kv_size(*strokes); i++) {
      remfmt_stroke *st = &kv_A(*strokes, i);
      float xOffset = (st->version == 6) ? ((float)DEV_W / 2.0f) : 0.0f;
      for (int j = 0; j < kv_size(st->segments); j++) {
        remfmt_seg sg = kv_A(st->segments, j);
        float x = sg.x + xOffset;
        float y = sg.y;
        if (x < min_x)
          min_x = x;
        if (x > max_x)
          max_x = x;
        if (y < min_y)
          min_y = y;
        if (y > max_y)
          max_y = y;
      }
    }
  }

  int port_w = (int)ceilf(max_x - min_x);
  int port_h = (int)ceilf(max_y - min_y);

  int width = port_w;
  int height = port_h;
  if (prm && prm->landscape) {
    width = port_h;
    height = port_w;
  }

  sds pdf_content = sdsempty();
  if (strokes != NULL) {
    for (int i = 0; i < kv_size(*strokes); i++) {
      remfmt_stroke st = kv_A(*strokes, i);
      set_pen_attr(&st);
      int num_points = kv_size(st.segments);
      if (num_points == 0)
        continue;

      uint32_t seg_color;
      if (st.has_custom_color) {
        seg_color = st.custom_color;
      } else {
        seg_color = (st.color < 14) // size of svg_color array
                        ? svg_color[st.color]
                        : 0x000000;
      }

      float r = ((seg_color >> 16) & 0xff) / 255.0f;
      float g = ((seg_color >> 8) & 0xff) / 255.0f;
      float b = (seg_color & 0xff) / 255.0f;

      float seg_width = st.calc_width, lsw = seg_width;
      float xOffset = (st.version == 6) ? ((float)DEV_W / 2.0f) : 0.0f;

      float seg_alpha = get_seg_alpha(&st, &kv_A(st.segments, 0));
      const char *gs_state = "/GS100";
      if (st.pen == ERASER || st.pen == ERASE_AREA) {
        gs_state = "/GS100";
      } else {
        if (seg_alpha < 0.15) {
          gs_state = "/GS10";
        } else if (seg_alpha < 0.35) {
          gs_state = "/GS25";
        } else if (seg_alpha < 0.95) {
          gs_state = "/GS90";
        }
      }

      pdf_content = sdscatprintf(pdf_content, "q\n");
      pdf_content = sdscatprintf(pdf_content, "%s gs\n", gs_state);
      pdf_content = sdscatprintf(pdf_content, "%.3f w\n", seg_width);
      pdf_content = sdscatprintf(pdf_content, "%.3f %.3f %.3f RG\n", r, g, b);
      pdf_content = sdscatprintf(pdf_content, "%d J\n", st.square_cap ? 2 : 1);
      pdf_content = sdscatprintf(pdf_content, "1 j\n");

      // First point
      remfmt_seg pt0 = kv_A(st.segments, 0);
      float x0 = pt0.x + xOffset - min_x;
      float y0 = pt0.y - min_y;
      if (prm && prm->landscape) {
        float rx = (float)port_h - y0;
        float ry = x0;
        x0 = rx;
        y0 = ry;
      }
      float yp0 = (float)height - y0;
      pdf_content = sdscatprintf(pdf_content, "%.3f %.3f m\n", x0, yp0);

      if (num_points == 1) {
        pdf_content = sdscatprintf(pdf_content, "%.3f %.3f l\n", x0, yp0);
      }

      for (int j = 1; j < num_points; j++) {
        remfmt_seg sg = kv_A(st.segments, j);
        float x = sg.x + xOffset - min_x;
        float y = sg.y - min_y;

        if (prm && prm->landscape) {
          float rx = (float)port_h - y;
          float ry = x;
          x = rx;
          y = ry;
        }

        float yp = (float)height - y;
        seg_width = get_seg_width(&st, &sg);

        pdf_content = sdscatprintf(pdf_content, "%.3f %.3f l\n", x, yp);
        if (fabsf(lsw - seg_width) > 0.08f * lsw) {
          pdf_content = sdscatprintf(pdf_content, "S\n");
          pdf_content = sdscatprintf(pdf_content, "%.3f w\n", seg_width);
          pdf_content = sdscatprintf(pdf_content, "%.3f %.3f m\n", x, yp);
          lsw = seg_width;
        }
      }
      pdf_content = sdscatprintf(pdf_content, "S\nQ\n");
    }
  }

  int tw = 0, th = 0;
  unsigned char *tdata = NULL;
  if (prm && prm->template_dir && prm->template_name &&
      prm->template_name[0] != '\0') {
    tdata = load_template_data(prm->template_dir, prm->template_name, &tw, &th);
  }

  sds pdf = sdsempty();
  pdf = sdscat(pdf, "%PDF-1.4\n");

  long offsets[6] = {0};

  offsets[1] = (long)sdslen(pdf);
  pdf = sdscat(pdf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

  offsets[2] = (long)sdslen(pdf);
  pdf = sdscat(
      pdf, "2 0 obj\n<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>\nendobj\n");

  offsets[3] = (long)sdslen(pdf);
  if (tdata) {
    pdf = sdscatprintf(
        pdf,
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %d "
        "%d ] /Contents 4 0 R /Resources << /XObject << /Im1 5 0 "
        "R >> /ExtGState << /GS25 << /Type /ExtGState /ca 0.25 /CA 0.25 >> "
        "/GS90 << /Type /ExtGState /ca 0.90 /CA 0.90 >> /GS10 << /Type "
        "/ExtGState /ca 0.10 /CA 0.10 >> /GS100 << /Type /ExtGState /ca 1.00 "
        "/CA 1.00 >> >> >> >>\nendobj\n",
        width, height);
  } else {
    pdf = sdscatprintf(
        pdf,
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %d "
        "%d ] /Contents 4 0 R /Resources << /ExtGState << /GS25 << /Type "
        "/ExtGState /ca 0.25 /CA 0.25 >> /GS90 << /Type /ExtGState /ca 0.90 "
        "/CA 0.90 >> /GS10 << /Type /ExtGState /ca 0.10 /CA 0.10 >> /GS100 << "
        "/Type /ExtGState /ca 1.00 /CA 1.00 >> >> >> >>\nendobj\n",
        width, height);
  }

  offsets[4] = (long)sdslen(pdf);

  if (tdata) {
    sds bg = sdsempty();
    if (prm && prm->landscape) {
      bg = sdscatprintf(bg, "q\n0 %d -%d 0 %d 0 cm\n/Im1 Do\nQ\n", height,
                        width, width);
    } else {
      bg = sdscatprintf(bg, "q\n%d 0 0 %d 0 0 cm\n/Im1 Do\nQ\n", width, height);
    }
    bg = sdscatsds(bg, pdf_content);
    sdsfree(pdf_content);
    pdf_content = bg;
  }

  pdf = sdscatprintf(
      pdf, "4 0 obj\n<< /Length %ld >>\nstream\n%s\nendstream\nendobj\n",
      (long)sdslen(pdf_content), pdf_content);

  if (tdata) {
    offsets[5] = (long)sdslen(pdf);
    pdf = sdscatprintf(
        pdf,
        "5 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length %ld >>\nstream\n",
        tw, th, (long)(tw * th * 3));
    pdf = sdscatlen(pdf, (const char *)tdata, tw * th * 3);
    pdf = sdscat(pdf, "\nendstream\nendobj\n");
    free(tdata);
  }

  long xref_pos = (long)sdslen(pdf);
  int num_objs = tdata ? 6 : 5;
  pdf = sdscatprintf(pdf, "xref\n0 %d\n", num_objs);
  pdf = sdscat(pdf, "0000000000 65535 f \n");
  for (int i = 1; i < num_objs; i++) {
    pdf = sdscatprintf(pdf, "%010ld 00000 n \n", offsets[i]);
  }

  pdf = sdscatprintf(pdf, "trailer\n<< /Size %d /Root 1 0 R >>\n", num_objs);
  pdf = sdscatprintf(pdf, "startxref\n%ld\n%%%%EOF\n", xref_pos);

  fwrite(pdf, 1, sdslen(pdf), stream);
  sdsfree(pdf);
  sdsfree(pdf_content);
}

void remfmt_render_notebook_pdf(FILE *stream, int num_pages,
                                remfmt_stroke_vec **pages_strokes,
                                remfmt_render_params **pages_prms) {
  if (num_pages <= 0)
    return;

  typedef struct {
    sds path;
    unsigned char *tdata;
    int tw;
    int th;
    int obj_id;
  } unique_template;

  int *page_obj_ids = malloc(num_pages * sizeof(int));
  int *contents_obj_ids = malloc(num_pages * sizeof(int));
  int *page_template_obj_ids = calloc(num_pages, sizeof(int));

  unique_template *utemplates = calloc(num_pages, sizeof(unique_template));
  int num_utemplates = 0;

  // 1. Gather all unique templates and load them once
  for (int i = 0; i < num_pages; i++) {
    remfmt_render_params *prm = pages_prms[i];
    if (prm && prm->template_dir && prm->template_name &&
        prm->template_name[0] != '\0') {
      sds tpl_key = sdscatprintf(sdsempty(), "%s/%s", prm->template_dir,
                                 prm->template_name);

      int found_idx = -1;
      for (int j = 0; j < num_utemplates; j++) {
        if (sdscmp(utemplates[j].path, tpl_key) == 0) {
          found_idx = j;
          break;
        }
      }

      if (found_idx == -1) {
        int tw = 0, th = 0;
        unsigned char *tdata =
            load_template_data(prm->template_dir, prm->template_name, &tw, &th);
        if (tdata != NULL) {
          utemplates[num_utemplates].path = sdsdup(tpl_key);
          utemplates[num_utemplates].tdata = tdata;
          utemplates[num_utemplates].tw = tw;
          utemplates[num_utemplates].th = th;
          utemplates[num_utemplates].obj_id = 0; // will assign later
          num_utemplates++;
        }
      }
      sdsfree(tpl_key);
    }
  }

  // 2. Assign PDF object IDs
  int next_id = 3;
  for (int i = 0; i < num_pages; i++) {
    page_obj_ids[i] = next_id++;
    contents_obj_ids[i] = next_id++;
  }

  for (int j = 0; j < num_utemplates; j++) {
    utemplates[j].obj_id = next_id++;
  }

  // Map each page to its unique template's PDF object ID
  for (int i = 0; i < num_pages; i++) {
    remfmt_render_params *prm = pages_prms[i];
    if (prm && prm->template_dir && prm->template_name &&
        prm->template_name[0] != '\0') {
      sds tpl_key = sdscatprintf(sdsempty(), "%s/%s", prm->template_dir,
                                 prm->template_name);
      for (int j = 0; j < num_utemplates; j++) {
        if (sdscmp(utemplates[j].path, tpl_key) == 0) {
          page_template_obj_ids[i] = utemplates[j].obj_id;
          break;
        }
      }
      sdsfree(tpl_key);
    }
  }

  int total_objs = next_id;
  long *offsets = calloc(total_objs, sizeof(long));

  sds pdf = sdsempty();
  pdf = sdscat(pdf, "%PDF-1.4\n");

  offsets[1] = (long)sdslen(pdf);
  pdf = sdscat(pdf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

  offsets[2] = (long)sdslen(pdf);
  pdf = sdscat(pdf, "2 0 obj\n<< /Type /Pages /Kids [ ");
  for (int i = 0; i < num_pages; i++) {
    pdf = sdscatprintf(pdf, "%d 0 R ", page_obj_ids[i]);
  }
  pdf = sdscatprintf(pdf, "] /Count %d >>\nendobj\n", num_pages);

  // 3. Render pages
  for (int i = 0; i < num_pages; i++) {
    remfmt_stroke_vec *strokes = pages_strokes[i];
    remfmt_render_params *prm = pages_prms[i];

    float min_x = 0.0f;
    float max_x = (float)DEV_W;
    float min_y = 0.0f;
    float max_y = (float)DEV_H;

    if (strokes != NULL && !(prm && prm->annotation)) {
      for (int k = 0; k < kv_size(*strokes); k++) {
        remfmt_stroke *st = &kv_A(*strokes, k);
        float xOffset = (st->version == 6) ? ((float)DEV_W / 2.0f) : 0.0f;
        for (int j = 0; j < kv_size(st->segments); j++) {
          remfmt_seg sg = kv_A(st->segments, j);
          float x = sg.x + xOffset;
          float y = sg.y;
          if (x < min_x)
            min_x = x;
          if (x > max_x)
            max_x = x;
          if (y < min_y)
            min_y = y;
          if (y > max_y)
            max_y = y;
        }
      }
    }

    int port_w = (int)ceilf(max_x - min_x);
    int port_h = (int)ceilf(max_y - min_y);
    int width = port_w;
    int height = port_h;
    if (prm && prm->landscape) {
      width = port_h;
      height = port_w;
    }

    sds pdf_content = sdsempty();
    if (strokes != NULL) {
      for (int k = 0; k < kv_size(*strokes); k++) {
        remfmt_stroke st = kv_A(*strokes, k);
        set_pen_attr(&st);
        int num_points = kv_size(st.segments);
        if (num_points == 0)
          continue;

        uint32_t seg_color;
        if (st.has_custom_color) {
          seg_color = st.custom_color;
        } else {
          seg_color = (st.color < 14) ? svg_color[st.color] : 0x000000;
        }

        float r = ((seg_color >> 16) & 0xff) / 255.0f;
        float g = ((seg_color >> 8) & 0xff) / 255.0f;
        float b = (seg_color & 0xff) / 255.0f;

        float seg_width = st.calc_width, lsw = seg_width;
        float xOffset = (st.version == 6) ? ((float)DEV_W / 2.0f) : 0.0f;

        float seg_alpha = get_seg_alpha(&st, &kv_A(st.segments, 0));
        const char *gs_state = "/GS100";
        if (seg_alpha < 0.15) {
          gs_state = "/GS10";
        } else if (seg_alpha < 0.35) {
          gs_state = "/GS25";
        } else if (seg_alpha < 0.95) {
          gs_state = "/GS90";
        }

        pdf_content = sdscatprintf(pdf_content, "q\n");
        pdf_content = sdscatprintf(pdf_content, "%s gs\n", gs_state);
        pdf_content = sdscatprintf(pdf_content, "%.3f w\n", seg_width);
        pdf_content = sdscatprintf(pdf_content, "%.3f %.3f %.3f RG\n", r, g, b);
        pdf_content =
            sdscatprintf(pdf_content, "%d J\n", st.square_cap ? 2 : 1);
        pdf_content = sdscatprintf(pdf_content, "1 j\n");

        remfmt_seg pt0 = kv_A(st.segments, 0);
        float x0 = pt0.x + xOffset - min_x;
        float y0 = pt0.y - min_y;
        if (prm && prm->landscape) {
          float rx = (float)port_h - y0;
          float ry = x0;
          x0 = rx;
          y0 = ry;
        }
        float yp0 = (float)height - y0;
        pdf_content = sdscatprintf(pdf_content, "%.3f %.3f m\n", x0, yp0);

        if (num_points == 1) {
          pdf_content = sdscatprintf(pdf_content, "%.3f %.3f l\n", x0, yp0);
        }

        for (int j = 1; j < num_points; j++) {
          remfmt_seg sg = kv_A(st.segments, j);
          float x = sg.x + xOffset - min_x;
          float y = sg.y - min_y;

          if (prm && prm->landscape) {
            float rx = (float)port_h - y;
            float ry = x;
            x = rx;
            y = ry;
          }

          float yp = (float)height - y;
          seg_width = get_seg_width(&st, &sg);

          pdf_content = sdscatprintf(pdf_content, "%.3f %.3f l\n", x, yp);
          if (fabsf(lsw - seg_width) > 0.08f * lsw) {
            pdf_content = sdscatprintf(pdf_content, "S\n");
            pdf_content = sdscatprintf(pdf_content, "%.3f w\n", seg_width);
            pdf_content = sdscatprintf(pdf_content, "%.3f %.3f m\n", x, yp);
            lsw = seg_width;
          }
        }
        pdf_content = sdscatprintf(pdf_content, "S\nQ\n");
      }
    }

    offsets[page_obj_ids[i]] = (long)sdslen(pdf);
    if (page_template_obj_ids[i] != 0) {
      pdf = sdscatprintf(
          pdf,
          "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %d "
          "%d ] /Contents %d 0 R /Resources << /XObject << /Im1 %d 0 "
          "R >> /ExtGState << /GS25 << /Type /ExtGState /ca 0.25 /CA 0.25 >> "
          "/GS90 << /Type /ExtGState /ca 0.90 /CA 0.90 >> /GS10 << /Type "
          "/ExtGState /ca 0.10 /CA 0.10 >> /GS100 << /Type /ExtGState /ca 1.00 "
          "/CA 1.00 >> >> >> >>\nendobj\n",
          page_obj_ids[i], width, height, contents_obj_ids[i],
          page_template_obj_ids[i]);
    } else {
      pdf = sdscatprintf(
          pdf,
          "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %d "
          "%d ] /Contents %d 0 R /Resources << /ExtGState << /GS25 << /Type "
          "/ExtGState /ca 0.25 /CA 0.25 >> /GS90 << /Type /ExtGState /ca 0.90 "
          "/CA 0.90 >> /GS10 << /Type /ExtGState /ca 0.10 /CA 0.10 >> /GS100 "
          "<< /Type /ExtGState /ca 1.00 /CA 1.00 >> >> >> >>\nendobj\n",
          page_obj_ids[i], width, height, contents_obj_ids[i]);
    }

    offsets[contents_obj_ids[i]] = (long)sdslen(pdf);
    if (page_template_obj_ids[i] != 0) {
      sds bg = sdsempty();
      if (prm && prm->landscape) {
        bg = sdscatprintf(bg, "q\n0 %d -%d 0 %d 0 cm\n/Im1 Do\nQ\n", height,
                          width, width);
      } else {
        bg = sdscatprintf(bg, "q\n%d 0 0 %d 0 0 cm\n/Im1 Do\nQ\n", width,
                          height);
      }
      bg = sdscatsds(bg, pdf_content);
      sdsfree(pdf_content);
      pdf_content = bg;
    }

    pdf = sdscatprintf(
        pdf, "%d 0 obj\n<< /Length %ld >>\nstream\n%s\nendstream\nendobj\n",
        contents_obj_ids[i], (long)sdslen(pdf_content), pdf_content);
    sdsfree(pdf_content);
  }

  // 4. Render unique template image objects
  for (int j = 0; j < num_utemplates; j++) {
    offsets[utemplates[j].obj_id] = (long)sdslen(pdf);
    pdf = sdscatprintf(
        pdf,
        "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length %ld >>\nstream\n",
        utemplates[j].obj_id, utemplates[j].tw, utemplates[j].th,
        (long)(utemplates[j].tw * utemplates[j].th * 3));
    pdf = sdscatlen(pdf, (const char *)utemplates[j].tdata,
                    utemplates[j].tw * utemplates[j].th * 3);
    pdf = sdscat(pdf, "\nendstream\nendobj\n");
  }

  long xref_pos = (long)sdslen(pdf);
  pdf = sdscatprintf(pdf, "xref\n0 %d\n", total_objs);
  pdf = sdscat(pdf, "0000000000 65535 f \n");
  for (int i = 1; i < total_objs; i++) {
    pdf = sdscatprintf(pdf, "%010ld 00000 n \n", offsets[i]);
  }

  pdf = sdscatprintf(pdf, "trailer\n<< /Size %d /Root 1 0 R >>\n", total_objs);
  pdf = sdscatprintf(pdf, "startxref\n%ld\n%%%%EOF\n", xref_pos);

  fwrite(pdf, 1, sdslen(pdf), stream);
  sdsfree(pdf);

  // 5. Cleanup
  for (int j = 0; j < num_utemplates; j++) {
    sdsfree(utemplates[j].path);
    free(utemplates[j].tdata);
  }
  free(utemplates);

  free(page_obj_ids);
  free(contents_obj_ids);
  free(page_template_obj_ids);
  free(offsets);
}
