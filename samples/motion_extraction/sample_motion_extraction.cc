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
#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/animation_optimizer.h"
#include "ozz/animation/offline/motion_extractor.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_track.h"
#include "ozz/animation/offline/track_builder.h"
#include "ozz/animation/offline/track_optimizer.h"
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
#include "ozz/base/maths/vec_float.h"
#include "ozz/options/options.h"

// Skeleton archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(skeleton,
                           "Path to the skeleton (ozz archive format).",
                           "media/skeleton.ozz", false)

// Animation archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(animation,
                           "Path to the animation (ozz archive format).",
                           "media/raw_animation.ozz", false)

class MotionSampleApplication : public ozz::sample::Application {
 public:
  MotionSampleApplication() {}

 protected:
  // Updates current animation time and skeleton pose.
  virtual bool OnUpdate(float _dt, float) {
    // Updates current animation time.
    controller_.Update(animation_, _dt);

    // Updates motion.
    //-------------------------------------------------------------------------

    // Reset character transform
    transform_ = ozz::math::Float4x4::identity();

    // Get position from motion track
    if (apply_motion_position_) {
      ozz::math::Float3 position;
      ozz::animation::Float3TrackSamplingJob position_sampler;
      position_sampler.track = &motion_track_.position;
      position_sampler.result = &position;
      position_sampler.ratio = controller_.time_ratio();
      if (!position_sampler.Run()) {
        return false;
      }

      transform_ =  // Apply motion position to character transform
          transform_ * ozz::math::Float4x4::Translation(position);
    }

    // Get rotation from motion track
    if (apply_motion_rotation_) {
      ozz::math::Quaternion rotation;
      ozz::animation::QuaternionTrackSamplingJob rotation_sampler;
      rotation_sampler.track = &motion_track_.rotation;
      rotation_sampler.result = &rotation;
      rotation_sampler.ratio = controller_.time_ratio();
      if (!rotation_sampler.Run()) {
        return false;
      }

      transform_ =  // Apply motion rotation to character transform
          transform_ * ozz::math::Float4x4::FromQuaternion(
                           ozz::math::simd_float4::LoadPtrU(&rotation.x));
    }

    // Updates animation.
    //-------------------------------------------------------------------------

    // Samples optimized animation at t = animation_time_.
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

    // Draw a box at character's root.
    const ozz::math::Float3 offset(
        0, motion_extractor_.position_settings.y ? -1.f : 0, 0);
    const ozz::math::Box box(ozz::math::Float3(-.25f, 0, -.25f) + offset,
                             ozz::math::Float3(.25f, 1.8f, .25f) + offset);
    success &= _renderer->DrawBoxIm(box, transform_, ozz::sample::kWhite);

    // Draw motion tracks.
    const float at = controller_.time_ratio();
    const float step = 1.f / (animation_.duration() * 60.f);
    success &=
        ozz::sample::DrawMotion(_renderer, motion_track_, 0.f, at, 1.f, step,
                                transform_, ozz::math::Quaternion::identity());
    return success;
  }

  bool ExtractMotion() {
    // Raw motion tracks extraction
    ozz::animation::offline::RawFloat3Track raw_motion_position;
    ozz::animation::offline::RawQuaternionTrack raw_motion_rotation;
    ozz::animation::offline::RawAnimation baked_animation;
    if (!motion_extractor_(raw_animation_, skeleton_, &raw_motion_position,
                           &raw_motion_rotation, &baked_animation)) {
      return false;
    }

    {  // Track optimization and runtime building
      ozz::animation::offline::TrackOptimizer optimizer;
      ozz::animation::offline::RawFloat3Track raw_track_position_opt;
      if (!optimizer(raw_motion_position, &raw_track_position_opt)) {
        return false;
      }

      ozz::animation::offline::RawQuaternionTrack raw_track_rotation_opt;
      if (!optimizer(raw_motion_rotation, &raw_track_rotation_opt)) {
        return false;
      }

      // Build runtime tracks
      ozz::animation::offline::TrackBuilder track_builder;
      auto position_track = track_builder(raw_track_position_opt);
      auto rotation_track = track_builder(raw_track_rotation_opt);
      if (!position_track || !rotation_track) {
        return false;
      }
      motion_track_.position = std::move(*position_track);
      motion_track_.rotation = std::move(*rotation_track);
    }

    {  // Optimizes and builds runtime animation
      ozz::animation::offline::RawAnimation baked_animation_opt;
      ozz::animation::offline::AnimationOptimizer optimizer;
      if (!optimizer(baked_animation, skeleton_, &baked_animation_opt)) {
        return false;
      }

      ozz::animation::offline::AnimationBuilder builder;
      auto animation = builder(baked_animation_opt);
      if (!animation) {
        return false;
      }
      animation_ = std::move(*animation);

      // Animation was changed, context needs to know.
      context_.Invalidate();
    }

    return true;
  }

  virtual bool OnInitialize() {
    // Reading skeleton.
    if (!ozz::sample::LoadSkeleton(OPTIONS_skeleton, &skeleton_)) {
      return false;
    }

    // Reading animation.
    if (!ozz::sample::LoadRawAnimation(OPTIONS_animation, &raw_animation_)) {
      return false;
    }

    // Setup default extraction for the sample.
    motion_extractor_.position_settings = {
        true, true, true,  // Components
        ozz::animation::offline::MotionExtractor::Reference::kAbsolute,
        true  // Bake
    };
    motion_extractor_.rotation_settings = {
        false, true, false,  // Components
        ozz::animation::offline::MotionExtractor::Reference::kAbsolute,
        true  // Bake
    };

    if (!ExtractMotion()) {
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
      bool rebuild = false;
      static bool open = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Motion extraction", &open);
      {
        static bool position = true;
        ozz::sample::ImGui::OpenClose ocp(_im_gui, "Position", &position);
        {
          ozz::sample::ImGui::OpenClose occ(_im_gui, "Components", nullptr);
          rebuild |=
              _im_gui->DoCheckBox("x", &motion_extractor_.position_settings.x);
          rebuild |=
              _im_gui->DoCheckBox("y", &motion_extractor_.position_settings.y);
          rebuild |=
              _im_gui->DoCheckBox("z", &motion_extractor_.position_settings.z);
        }

        {
          ozz::sample::ImGui::OpenClose ocr(_im_gui, "Reference", nullptr);
          int ref =
              static_cast<int>(motion_extractor_.position_settings.reference);
          rebuild |= _im_gui->DoRadioButton(0, "Identity", &ref);
          rebuild |= _im_gui->DoRadioButton(1, "Skeleton", &ref);
          rebuild |= _im_gui->DoRadioButton(2, "Animation", &ref);
          motion_extractor_.position_settings.reference =
              static_cast<ozz::animation::offline::MotionExtractor::Reference>(
                  ref);
        }
        rebuild |= _im_gui->DoCheckBox(
            "Bake", &motion_extractor_.position_settings.bake);
        rebuild |= _im_gui->DoCheckBox(
            "Loop", &motion_extractor_.position_settings.loop);
      }

      {
        static bool rotation = true;
        ozz::sample::ImGui::OpenClose ocp(_im_gui, "Rotation", &rotation);
        {
          ozz::sample::ImGui::OpenClose occ(_im_gui, "Components", nullptr);
          rebuild |= _im_gui->DoCheckBox(
              "x / pitch", &motion_extractor_.rotation_settings.x);
          rebuild |= _im_gui->DoCheckBox(
              "y / yaw", &motion_extractor_.rotation_settings.y);
          rebuild |= _im_gui->DoCheckBox(
              "z / roll", &motion_extractor_.rotation_settings.z);
        }

        {
          ozz::sample::ImGui::OpenClose ocr(_im_gui, "Reference", nullptr);
          int ref =
              static_cast<int>(motion_extractor_.rotation_settings.reference);
          rebuild |= _im_gui->DoRadioButton(0, "Identity", &ref);
          rebuild |= _im_gui->DoRadioButton(1, "Skeleton", &ref);
          rebuild |= _im_gui->DoRadioButton(2, "Animation", &ref);
          motion_extractor_.rotation_settings.reference =
              static_cast<ozz::animation::offline::MotionExtractor::Reference>(
                  ref);
        }
        rebuild |= _im_gui->DoCheckBox(
            "Bake", &motion_extractor_.rotation_settings.bake);
        rebuild |= _im_gui->DoCheckBox(
            "Loop", &motion_extractor_.rotation_settings.loop);
      }

      if (rebuild) {
        if (!ExtractMotion()) {
          return false;
        }
      }
    }

    {
      static bool open = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Motion control", &open);
      if (open) {
        _im_gui->DoCheckBox("Use motion position", &apply_motion_position_);
        _im_gui->DoCheckBox("Use motion rotation", &apply_motion_rotation_);
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

  // Store extractor to expose parameters to GUI.
  ozz::animation::offline::MotionExtractor motion_extractor_;

  // Runtime skeleton.
  ozz::animation::Skeleton skeleton_;

  // Original animation.
  ozz::animation::offline::RawAnimation raw_animation_;

  // Runtime animation.
  ozz::animation::Animation animation_;

  // Runtime motion tracks.
  ozz::sample::MotionTrack motion_track_;

  // Sampling context.
  ozz::animation::SamplingJob::Context context_;

  // Character transform.
  ozz::math::Float4x4 transform_;

  // Buffer of local transforms as sampled from animation_.
  ozz::vector<ozz::math::SoaTransform> locals_;

  // Buffer of model space matrices.
  ozz::vector<ozz::math::Float4x4> models_;

  // GUI options to apply root motion.
  bool apply_motion_position_ = true;
  bool apply_motion_rotation_ = true;
};

int main(int _argc, const char** _argv) {
  const char* title = "Ozz-animation sample: Root motion extraction";
  return MotionSampleApplication().Run(_argc, _argv, "1.0", title);
}
