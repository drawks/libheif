/*
 * HEIF codec.
 * Copyright (c) 2017 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include "heif_file.h"

#include <fstream>
#include <iostream>
#include <assert.h>

extern "C" {
#include <libde265/de265.h>
}


using namespace heif;


HeifFile::HeifFile()
{
}


HeifFile::~HeifFile()
{
}


std::vector<uint32_t> HeifFile::get_image_IDs() const
{
  std::vector<uint32_t> IDs;

  for (const auto& image : m_images) {
    IDs.push_back(image.second.m_infe_box->get_item_ID());
  }

  return IDs;
}


Error HeifFile::read_from_file(const char* input_filename)
{
  std::ifstream istr(input_filename);

  uint64_t maxSize = std::numeric_limits<uint64_t>::max();
  heif::BitstreamRange range(&istr, maxSize);


  Error error = parse_heif_file(range);
  return error;
}


Error HeifFile::parse_heif_file(BitstreamRange& range)
{
  // --- read all top-level boxes

  for (;;) {
    std::shared_ptr<Box> box;
    Error error = Box::read(range, &box);
    if (error != Error::OK || range.error() || range.eof()) {
      break;
    }

    m_top_level_boxes.push_back(box);


    // dump box content for debugging

    heif::Indent indent;
    std::cout << "\n";
    std::cout << box->dump(indent);


    // extract relevant boxes (ftyp, meta)

    if (box->get_short_type() == fourcc("meta")) {
      m_meta_box = std::dynamic_pointer_cast<Box_meta>(box);
    }

    if (box->get_short_type() == fourcc("ftyp")) {
      m_ftyp_box = std::dynamic_pointer_cast<Box_ftyp>(box);
    }
  }



  // --- check whether this is a HEIF file and its structural format

  if (!m_ftyp_box) {
    return Error(Error::InvalidInput);
  }

  if (!m_ftyp_box->has_compatible_brand(fourcc("heic"))) {
    return Error(Error::NoCompatibleBrandType);
  }

  if (!m_meta_box) {
    return Error(Error::InvalidInput, Error::NoMetaBox);
    // fprintf(stderr, "Not a valid HEIF file (no 'meta' box found)\n");
  }


  auto hdlr_box = std::dynamic_pointer_cast<Box_hdlr>(m_meta_box->get_child_box(fourcc("hdlr")));
  if (!hdlr_box) {
    return Error(Error::InvalidInput, Error::NoHdlrBox);
  }

  if (hdlr_box->get_handler_type() != fourcc("pict")) {
    return Error(Error::InvalidInput, Error::NoPictHandler);
  }


  // --- find mandatory boxes needed for image decoding

  auto pitm_box = std::dynamic_pointer_cast<Box_pitm>(m_meta_box->get_child_box(fourcc("pitm")));
  if (!pitm_box) {
    return Error(Error::InvalidInput, Error::NoPitmBox);
  }

  std::shared_ptr<Box> iprp_box = m_meta_box->get_child_box(fourcc("iprp"));
  if (!iprp_box) {
    return Error(Error::InvalidInput, Error::NoIprpBox);
  }

  m_ipco_box = std::dynamic_pointer_cast<Box_ipco>(iprp_box->get_child_box(fourcc("ipco")));
  if (!m_ipco_box) {
    return Error(Error::InvalidInput, Error::NoIpcoBox);
  }

  m_ipma_box = std::dynamic_pointer_cast<Box_ipma>(iprp_box->get_child_box(fourcc("ipma")));
  if (!m_ipma_box) {
    return Error(Error::InvalidInput, Error::NoIpmaBox);
  }

  m_iloc_box = std::dynamic_pointer_cast<Box_iloc>(m_meta_box->get_child_box(fourcc("iloc")));
  if (!m_iloc_box) {
    return Error(Error::InvalidInput, Error::NoIlocBox);
  }

  std::shared_ptr<Box> iinf_box = m_meta_box->get_child_box(fourcc("iinf"));
  if (!iinf_box) {
    return Error(Error::InvalidInput, Error::NoIinfBox);
  }



  // --- build list of images

  m_primary_image_ID = pitm_box->get_item_ID();

  std::vector<std::shared_ptr<Box>> infe_boxes = iinf_box->get_child_boxes(fourcc("infe"));

  for (auto& box : infe_boxes) {
    std::shared_ptr<Box_infe> infe_box = std::dynamic_pointer_cast<Box_infe>(box);
    assert(infe_box);

    Image img;
    img.m_infe_box = infe_box;

    m_images.insert( std::make_pair(infe_box->get_item_ID(), img) );
  }

#if 0
  // HEVC image headers.
  std::vector<std::shared_ptr<Box>> hvcC_boxes = ipco_box->get_child_boxes(fourcc("hvcC"));
  if (hvcC_boxes.empty()) {
    // No images in the file.
    images->clear();
    return true;
  }

  // HEVC image data.
  std::shared_ptr<Box_iloc> iloc = std::dynamic_pointer_cast<Box_iloc>(get_child_box(fourcc("iloc")));
  if (!iloc || iloc->get_items().size() != hvcC_boxes.size()) {
    // TODO(jojo): Can images share a header?
    return false;
  }

  const std::vector<Box_iloc::Item>& iloc_items = iloc->get_items();
  for (size_t i = 0; i < hvcC_boxes.size(); i++) {
    Box_hvcC* hvcC = static_cast<Box_hvcC*>(hvcC_boxes[i].get());
    std::vector<uint8_t> data;
    if (!hvcC->get_headers(&data)) {
      return false;
    }
    if (!iloc->read_data(iloc_items[i], istr, &data)) {
      return false;
    }

    images->push_back(std::move(data));
  }

  return true;
#endif


  return Error::OK;
}


Error HeifFile::get_image(uint16_t ID, const struct de265_image** img, std::istream& TODO_istr) const
{
  assert(img);


  // --- get the image from the list of all images

  auto image_iter = m_images.find(ID);
  if (image_iter == m_images.end()) {
    return Error(Error::NonexistingImage);
  }

  const Image& image = image_iter->second;


  // --- get properties for this image

  std::vector< std::shared_ptr<Box> > properties;

  const std::vector<Box_ipma::PropertyAssociation>* property_assoc = m_ipma_box->get_properties_for_item_ID(ID);
  if (property_assoc == nullptr) {
    return Error(Error::InvalidInput, Error::NoPropertiesForItemID);
  }

  auto allProperties = m_ipco_box->get_all_child_boxes();
  for (const  Box_ipma::PropertyAssociation& assoc : *property_assoc) {
    if (assoc.property_index > allProperties.size()) {
      return Error(Error::InvalidInput, Error::NonexistingPropertyReferenced);
    }

    if (assoc.property_index > 0) {
      properties.push_back(allProperties[assoc.property_index - 1]);
    }

    // TODO: essential flag ?
  }



  // --- decode image, depending on its type

  std::string item_type = image.m_infe_box->get_item_type();

  if (item_type == "hvc1") {
    // --- --- --- HEVC

    // --- get codec configuration

    std::shared_ptr<Box_hvcC> hvcC_box;
    for (auto& prop_box : properties) {
      if (prop_box->get_short_type() == fourcc("hvcC")) {
        hvcC_box = std::dynamic_pointer_cast<Box_hvcC>(prop_box);
        assert(hvcC_box);
      }
    }

    // --- get coded image data pointers

    auto items = m_iloc_box->get_items();
    const Box_iloc::Item* item = nullptr;
    for (const auto& i : items) {
      if (i.item_ID == ID) {
        item = &i;
        break;
      }
    }

    if (!item) {
      return Error(Error::InvalidInput, Error::NoInputDataInFile);
    }


    std::vector<uint8_t> data;
    if (!hvcC_box->get_headers(&data)) {
      // TODO
    }

    if (!m_iloc_box->read_data(*item, TODO_istr, &data)) {
      // TODO
    }



    // --- decode HEVC image with libde265

    de265_decoder_context* ctx = de265_new_decoder();
    de265_start_worker_threads(ctx,1);

    de265_push_data(ctx, data.data(), data.size(), 0, nullptr);

#if LIBDE265_NUMERIC_VERSION >= 0x02000000
    de265_push_end_of_stream(ctx);
#else
    de265_flush_data(ctx);
#endif


#if LIBDE265_NUMERIC_VERSION >= 0x02000000
    int action = de265_get_action(ctx, 1);
    printf("libde265 action: %d\n",action);

    if (action==de265_action_get_image) {
      printf("image decoded !\n");

      *img = de265_get_next_picture(ctx);
    }
#else
    int more;
    de265_error err;
    do {
      more = 0;
      err = de265_decode(ctx, &more);
      if (err != DE265_OK) {
        printf("Error decoding: %s (%d)\n", de265_get_error_text(err), err);
        break;
      }

      const struct de265_image* image = de265_get_next_picture(ctx);
      if (image) {
        *img = image;

        printf("Decoded image: %d/%d\n", de265_get_image_width(image, 0),
            de265_get_image_height(image, 0));
        de265_release_next_picture(ctx);
      }
    } while (more);
#endif


    FILE* fh = fopen("out.bin", "wb");
    fwrite(data.data(), 1, data.size(), fh);
    fclose(fh);
  }
  else {
    return Error(Error::Unsupported, Error::UnsupportedImageType);
  }

#if 0
  // HEVC image headers.
  std::vector<std::shared_ptr<Box>> hvcC_boxes = ipco_box->get_child_boxes(fourcc("hvcC"));
  if (hvcC_boxes.empty()) {
    // No images in the file.
    images->clear();
    return true;
  }

  // HEVC image data.
  std::shared_ptr<Box_iloc> iloc = std::dynamic_pointer_cast<Box_iloc>(get_child_box(fourcc("iloc")));
  if (!iloc || iloc->get_items().size() != hvcC_boxes.size()) {
    // TODO(jojo): Can images share a header?
    return false;
  }

  const std::vector<Box_iloc::Item>& iloc_items = iloc->get_items();
  for (size_t i = 0; i < hvcC_boxes.size(); i++) {
    Box_hvcC* hvcC = static_cast<Box_hvcC*>(hvcC_boxes[i].get());
    std::vector<uint8_t> data;
    if (!hvcC->get_headers(&data)) {
      return false;
    }
    if (!iloc->read_data(iloc_items[i], istr, &data)) {
      return false;
    }

    images->push_back(std::move(data));
  }
#endif

  return Error::OK;
}