#include "scanner/api/kernel.h"
#include "scanner/api/op.h"
#include "scanner/types.pb.h"
#include "scanner/util/bbox.h"
#include "scanner/util/opencv.h"
#include "scanner/util/serialize.h"
#include "stdlib/stdlib.pb.h"

namespace scanner {

#define CLASSES 81
#define SCORE_THRESHOLD 0.7
#define BOX_SIZE 5
#define FEATURES 4096

class FasterRCNNOutputKernel : public Kernel {
 public:
  FasterRCNNOutputKernel(const Kernel::Config& config) : Kernel(config) {}

  void execute(const BatchedColumns& input_columns,
               BatchedColumns& output_columns) override {
    assert(input_columns.size() == 3);

    i32 input_count = input_columns[0].rows.size();
    i32 cls_prob_idx = 0;
    i32 rois_idx = 1;
    i32 fc7_idx = 2;
    const std::vector<Row>&cls_prob = input_columns[cls_prob_idx].rows,
          &rois = input_columns[rois_idx].rows,
          &fc7 = input_columns[fc7_idx].rows;

    for (i32 i = 0; i < input_count; ++i) {
      i32 proposal_count =
          input_columns[rois_idx].rows[i].size / (BOX_SIZE * sizeof(f32));
      assert(rois[i].size == BOX_SIZE * sizeof(f32) * proposal_count);
      assert(cls_prob[i].size == CLASSES * sizeof(f32) * proposal_count);
      std::vector<BoundingBox> bboxes;
      for (i32 j = 0; j < proposal_count; ++j) {
        f32* roi = (f32*)(rois[i].buffer + (j * BOX_SIZE * sizeof(f32)));
        f32 x1 = roi[1], y1 = roi[2], x2 = roi[3], y2 = roi[4];

        BoundingBox bbox;
        bbox.set_x1(x1);
        bbox.set_y1(y1);
        bbox.set_x2(x2);
        bbox.set_y2(y2);

        f32 max_score = std::numeric_limits<f32>::min();
        i32 max_cls = 0;
        // Start at cls = 1 to skip background
        for (i32 cls = 1; cls < CLASSES; ++cls) {
          f32* scores =
              (f32*)(cls_prob[i].buffer + (j * CLASSES * sizeof(f32)));
          f32 score = scores[cls];
          if (score > max_score) {
            max_score = score;
            max_cls = cls;
          }
        }

        if (max_score > SCORE_THRESHOLD) {
          assert(max_cls != 0);
          bbox.set_score(max_score);
          bbox.set_track_id(j);
          bbox.set_label(max_cls);
          bboxes.push_back(bbox);
        }
      }

      std::vector<BoundingBox> best_bboxes;
      best_bboxes = best_nms(bboxes, 0.3);

      {
        size_t size;
        u8* buffer;
        serialize_bbox_vector(best_bboxes, buffer, size);
        output_columns[0].rows.push_back(Row{buffer, size});
      }

      {
        size_t size =
            std::max(best_bboxes.size() * FEATURES * sizeof(f32), (size_t)1);
        u8* buffer = new_buffer(CPU_DEVICE, size);
        for (i32 k = 0; k < best_bboxes.size(); ++k) {
          i32 j = best_bboxes[k].track_id();
          f32* fvec = (f32*)(fc7[i].buffer + (j * FEATURES * sizeof(f32)));
          std::memcpy(buffer + (k * FEATURES * sizeof(f32)), fvec,
                      FEATURES * sizeof(f32));
        }
        output_columns[1].rows.push_back(Row{buffer, size});
      }
    }
  }
};

REGISTER_OP(FasterRCNNOutput)
    .inputs({"caffe_output"})
    .outputs({"bboxes", "features"});
REGISTER_KERNEL(FasterRCNNOutput, FasterRCNNOutputKernel)
    .device(DeviceType::CPU)
    .num_devices(1);
}
