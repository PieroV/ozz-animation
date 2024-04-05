//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) Guillaume Blanc                                              //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "framework/application.h"
#include "framework/imgui.h"
#include "framework/motion_utils.h"
#include "framework/renderer.h"
#include "framework/utils.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/track.h"
#include "ozz/animation/runtime/track_sampling_job.h"
#include "ozz/base/log.h"
#include "ozz/base/maths/box.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"
#include "ozz/options/options.h"

// Skeleton archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(skeleton,
                           "Path to the skeleton (ozz archive format).",
                           "media/skeleton.ozz", false)

// Animation archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(animation,
                           "Path to the animation (ozz archive format).",
                           "media/animation.ozz", false)

// Motion tracks archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(motion,
                           "Path to the motion tracks (ozz archive format).",
                           "media/motion.ozz", false)

class MotionPlaybackSampleApplication : public ozz::sample::Application {
 protected:
  // Updates current animation time and skeleton pose.
  virtual bool OnUpdate(float _dt, float) {
    // Updates current animation time.
    int loops = controller_.Update(animation_, _dt);

    // Updates motion.
    //-------------------------------------------------------------------------

    // Updates motion accumulator.
    if (!motion_accumulator_.Update(motion_track_, controller_.time_ratio(),
                                    loops)) {
      return false;
    }

    // Updates the character transform matrix.
    const auto& transform = motion_accumulator_.transform();
    transform_ = ozz::math::Float4x4::FromAffine(
        apply_motion_position_ ? transform.translation
                               : ozz::math::Float3::zero(),
        apply_motion_rotation_ ? transform.rotation
                               : ozz::math::Quaternion::identity(),
        transform.scale);

    // Updates animation.
    //-------------------------------------------------------------------------

    // Samples optimized animation.
    ozz::animation::SamplingJob sampling_job;
    sampling_job.animation = &animation_;
    sampling_job.context = &context_;
    sampling_job.ratio = controller_.time_ratio();
    sampling_job.output = make_span(locals_);
    if (!sampling_job.Run()) {
      return false;
    }

    // Converts from local space to model space matrices.
    ozz::animation::LocalToModelJob ltm_job;
    ltm_job.skeleton = &skeleton_;
    ltm_job.input = make_span(locals_);
    ltm_job.output = make_span(models_);
    if (!ltm_job.Run()) {
      return false;
    }

    return true;
  }

  virtual bool OnDisplay(ozz::sample::Renderer* _renderer) {
    bool success = true;
    success &=
        _renderer->DrawPosture(skeleton_, make_span(models_), transform_);

    if (show_box_) {
      const ozz::math::Box bound(ozz::math::Float3(-.3f, 0, -.2f),
                                 ozz::math::Float3(.3f, 1.8f, .2f));
      success &= _renderer->DrawBoxIm(bound, transform_, ozz::sample::kWhite);
    }

    if (show_motion_) {
      success &= ozz::sample::DrawMotion(_renderer, motion_track_,
                                         controller_.time_ratio(),
                                         animation_.duration(), transform_);
    }

    return success;
  }

  virtual bool OnInitialize() {
    // Reading skeleton.
    if (!ozz::sample::LoadSkeleton(OPTIONS_skeleton, &skeleton_)) {
      return false;
    }

    // Reading animation.
    if (!ozz::sample::LoadAnimation(OPTIONS_animation, &animation_)) {
      return false;
    }

    // Reading motion tracks.
    if (!ozz::sample::LoadMotionTrack(OPTIONS_motion, &motion_track_.position,
                                      &motion_track_.rotation)) {
      return false;
    }

    // Skeleton and animation needs to match.
    if (skeleton_.num_joints() != animation_.num_tracks()) {
      return false;
    }

    // Allocates runtime buffers.
    const int num_soa_joints = skeleton_.num_soa_joints();
    locals_.resize(num_soa_joints);
    const int num_joints = skeleton_.num_joints();
    models_.resize(num_joints);

    // Allocates a context that matches animation requirements.
    context_.Resize(num_joints);

    return true;
  }

  virtual void OnDestroy() {}

  virtual bool OnGui(ozz::sample::ImGui* _im_gui) {
    // Exposes animation runtime playback controls.
    {
      static bool open = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Animation control", &open);
      if (open) {
        controller_.OnGui(animation_, _im_gui);
      }
    }

    {
      static bool open = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Motion control", &open);
      if (open) {
        _im_gui->DoCheckBox("Use motion position", &apply_motion_position_);
        _im_gui->DoCheckBox("Use motion rotation", &apply_motion_rotation_);
        _im_gui->DoCheckBox("Show box", &show_box_);
        _im_gui->DoCheckBox("Show motion", &show_motion_);

        if (_im_gui->DoButton("Reset accumulator")) {
          motion_accumulator_.Teleport(ozz::math::Transform::identity());
        }
      }
    }
    return true;
  }

  virtual void GetSceneBounds(ozz::math::Box* _bound) const {
    ozz::sample::ComputePostureBounds(make_span(models_), transform_, _bound);
  }

 private:
  // Playback animation controller. This is a utility class that helps with
  // controlling animation playback time.
  ozz::sample::PlaybackController controller_;

  // Runtime skeleton.
  ozz::animation::Skeleton skeleton_;

  // Runtime animation.
  ozz::animation::Animation animation_;

  // Position and rotation motion tracks
  ozz::sample::MotionTrack motion_track_;

  // Motion accumulator helper
  ozz::sample::MotionAccumulator motion_accumulator_;

  // Character transform
  ozz::math::Float4x4 transform_;

  // Sampling context.
  ozz::animation::SamplingJob::Context context_;

  // Buffer of local transforms as sampled from animation_.
  ozz::vector<ozz::math::SoaTransform> locals_;

  // Buffer of model space matrices.
  ozz::vector<ozz::math::Float4x4> models_;

  // Show box at root transform
  bool show_box_ = true;
  bool show_motion_ = true;

  // GUI options to apply root motion.
  bool apply_motion_position_ = true;
  bool apply_motion_rotation_ = true;
};

int main(int _argc, const char** _argv) {
  const char* title = "Ozz-animation sample: Motion root playback";
  return MotionPlaybackSampleApplication().Run(_argc, _argv, "1.0", title);
}
