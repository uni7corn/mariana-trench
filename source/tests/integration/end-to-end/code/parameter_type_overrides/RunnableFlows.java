/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.marianatrench.integrationtests;

interface RunnableWithArgument {
  void run(Object input);
}

public class RunnableFlows {

  static void noFlow() {
    Object data = Origin.source();
    runRunnable(
        new RunnableWithArgument() {
          @Override
          public void run(Object data) {
            // No data into sink.
          }
        },
        data);
  }

  static void flow() {
    Object data = Origin.source();
    runRunnable(
        new RunnableWithArgument() {
          @Override
          public void run(Object data) {
            Origin.sink(data);
          }
        },
        data);
  }

  static void genericInvocation(RunnableWithArgument runnable) {
    Object data = Origin.source();
    runRunnable(runnable, data);
  }

  static void runRunnable(RunnableWithArgument runnable, Object data) {
    // Run run run...
    runnable.run(data);

    // Issue - Only reported on method without parameter type overrides.
    Origin.sink(Origin.source());
  }

  static void sink(RunnableWithArgument runnable) {}

  static RunnableWithArgument source(RunnableWithArgument runnable) {
    return runnable;
  }

  static void testUserDefinedModelsOnParameterTypeOverrides() {
    // Expect issue: FN as parameter type overrides do not inherit
    // user defined models from the orignal method.
    sink(
        source(
            new RunnableWithArgument() {
              @Override
              public void run(Object data) {}
            }));
  }
}
