// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(async function() {
  TestRunner.addResult(`Tests live edit feature.\n`);
  await TestRunner.loadModule('sources_test_runner');
  await TestRunner.showPanel('sources');
  await TestRunner.addScriptTag('resources/edit-me.js');
  await TestRunner.addScriptTag('resources/edit-me-2.js');
  await TestRunner.addScriptTag('resources/edit-me-when-paused.js');

  var panel = UI.panels.sources;

  SourcesTestRunner.runDebuggerTestSuite([
    function testLiveEdit(next) {
      SourcesTestRunner.showScriptSource('edit-me.js', didShowScriptSource);

      function didShowScriptSource(sourceFrame) {
        replaceInSource(
            sourceFrame, 'return 0;', 'return "live-edited string";',
            didEditScriptSource);
      }

      function didEditScriptSource() {
        TestRunner.evaluateInPage('f()', didEvaluateInPage);
      }

      function didEvaluateInPage(result) {
        TestRunner.assertEquals(
            'live-edited string', result.description,
            'edited function returns wrong result');
        SourcesTestRunner.dumpSourceFrameContents(panel.visibleView);
        next();
      }
    },

    async function testLiveEditSyntaxError(next) {
      await TestRunner.addScriptTag('resources/edit-me-syntax-error.js');
      SourcesTestRunner.showScriptSource(
          'edit-me-syntax-error.js', didShowScriptSource);

      function didShowScriptSource(sourceFrame) {
        SourcesTestRunner.replaceInSource(
            sourceFrame, ',"I\'m good"', '"I\'m good"');
        SourcesTestRunner.dumpSourceFrameContents(panel.visibleView);
        next();
      }
    },

    function testLiveEditWhenPaused(next) {
      SourcesTestRunner.showScriptSource(
          'edit-me-when-paused.js', didShowScriptSource);

      function didShowScriptSource(sourceFrame) {
        SourcesTestRunner.waitUntilPaused(paused);
        TestRunner.evaluateInPage('f1()', didEvaluateInPage);
      }

      function paused(callFrames) {
        replaceInSource(
            panel.visibleView, 'return 1;', 'return 2;\n\n\n\n',
            didEditScriptSource);
      }

      function didEditScriptSource() {
        SourcesTestRunner.resumeExecution();
      }

      function didEvaluateInPage(result) {
        TestRunner.assertEquals(
            '3', result.description, 'edited function returns wrong result');
        next();
      }
    },

    function testNoCrashWhenOnlyOneFunctionOnStack(next) {
      SourcesTestRunner.showScriptSource(
          'edit-me-when-paused.js', didShowScriptSource);

      function didShowScriptSource(sourceFrame) {
        SourcesTestRunner.waitUntilPaused(paused);
        TestRunner.evaluateInPage('setTimeout(f1, 0)');
      }

      function paused(callFrames) {
        SourcesTestRunner.captureStackTrace(callFrames);
        replaceInSource(
            panel.visibleView, 'debugger;', 'debugger;\n', didEditScriptSource);
      }

      function didEditScriptSource() {
        SourcesTestRunner.resumeExecution(
            SourcesTestRunner.waitUntilPaused.bind(
                SourcesTestRunner,
                SourcesTestRunner.resumeExecution.bind(
                    SourcesTestRunner, next)));
      }
    },

    function testBreakpointsUpdated(next) {
      var testSourceFrame;
      SourcesTestRunner.showScriptSource('edit-me.js', didShowScriptSource);

      function didShowScriptSource(sourceFrame) {
        testSourceFrame = sourceFrame;
        SourcesTestRunner.waitJavaScriptSourceFrameBreakpoints(sourceFrame)
            .then(breakpointAdded);
        SourcesTestRunner.setBreakpoint(sourceFrame, 2, '', true);
      }

      function breakpointAdded() {
        replaceInSource(
            panel.visibleView, 'function f()', 'var a = 1;\nfunction f()',
            didEditScriptSource);
      }

      function didEditScriptSource() {
        SourcesTestRunner.waitJavaScriptSourceFrameBreakpoints(testSourceFrame)
            .then(
                () => SourcesTestRunner.dumpJavaScriptSourceFrameBreakpoints(
                    testSourceFrame))
            .then(
                () => Bindings.breakpointManager._allBreakpoints().map(
                    breakpoint => breakpoint.remove()))
            .then(next);
      }
    },

    function testNoCrashWhenLiveEditOnBreakpoint(next) {
      SourcesTestRunner.showScriptSource('edit-me-2.js', didShowScriptSource);

      var testSourceFrame;

      function didShowScriptSource(sourceFrame) {
        testSourceFrame = sourceFrame;
        SourcesTestRunner.waitJavaScriptSourceFrameBreakpoints(testSourceFrame)
            .then(breakpointAdded);
        SourcesTestRunner.setBreakpoint(sourceFrame, 2, '', true);
      }

      function breakpointAdded() {
        SourcesTestRunner.waitUntilPaused(pausedInF);
        TestRunner.evaluateInPage('setTimeout(editMe2F, 0)');
      }

      function pausedInF(callFrames) {
        replaceInSource(
            panel.visibleView, 'function editMe2F()', 'function editMe2F()\n',
            didEditScriptSource);
      }

      function didEditScriptSource() {
        SourcesTestRunner.resumeExecution(resumed);
      }

      function resumed() {
        next();
      }
    }
  ]);

  function replaceInSource(sourceFrame, string, replacement, callback) {
    TestRunner.addSniffer(
        TestRunner.debuggerModel, '_didEditScriptSource', callback);
    SourcesTestRunner.replaceInSource(sourceFrame, string, replacement);
    SourcesTestRunner.commitSource(sourceFrame);
  }
})();
