// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(async function() {
  TestRunner.addResult(`Test screen orientation override.\n`);
  await TestRunner.loadModule('console_test_runner');

  TestRunner.printDevToolsConsole();

  await TestRunner.navigatePromise('resources/screen-orientation-resource.html');

  Protocol.InspectorBackend.Options.suppressRequestErrors = false;
  function addDumpResult(next) {
    TestRunner.evaluateInPage('dump()', dumpCallback);

    function dumpCallback(result) {
      TestRunner.addResult(result.value);
      next();
    }
  }

  function testOverride(angle, orientation, next) {
    ConsoleTestRunner.addConsoleSniffer(addDumpResult.bind(null, next));
    TestRunner.EmulationAgent.invoke_setDeviceMetricsOverride({
      width: 0,
      height: 0,
      deviceScaleFactor: 0,
      mobile: true,
      fitWindow: false,
      screenOrientation: {type: orientation, angle: angle}
    });
  }

  async function testError(angle, orientation, next) {
    await TestRunner.EmulationAgent.invoke_setDeviceMetricsOverride({
      width: 0,
      height: 0,
      deviceScaleFactor: 0,
      mobile: true,
      fitWindow: false,
      screenOrientation: {type: orientation, angle: angle}
    });
    next();
  }

  var original;

  TestRunner.runTestSuite([
    function initial(next) {
      TestRunner.evaluateInPage('dump()', dumpCallback);

      function dumpCallback(result) {
        original = result.value;
        next();
      }
    },

    function setWrongAngle1(next) {
      testError(-1, 'portraitPrimary', next);
    },

    function setWrongAngle2(next) {
      testError(360, 'portraitPrimary', next);
    },

    function setWrongType(next) {
      testError(120, 'wrongType', next);
    },

    function setPortraitPrimary(next) {
      testOverride(0, 'portraitPrimary', next);
    },

    function setPortraitSecondary(next) {
      testOverride(180, 'portraitSecondary', next);
    },

    function setLandscapePrimary(next) {
      testOverride(90, 'landscapePrimary', next);
    },

    function restoresAfterReload(next) {
      TestRunner.reloadPage(addDumpResult.bind(null, next));
    },

    function setLandscapeSecondary(next) {
      testOverride(270, 'landscapeSecondary', next);
    },

    function clearOverride(next) {
      TestRunner.EmulationAgent.clearDeviceMetricsOverride().then(checkInitial);

      function checkInitial() {
        TestRunner.evaluateInPage('dump()', dumpCallback);
      }

      function dumpCallback(result) {
        TestRunner.addResult('Equals to initial: ' + (original === result.value ? 'true' : 'false'));
        next();
      }
    }
  ]);
})();
