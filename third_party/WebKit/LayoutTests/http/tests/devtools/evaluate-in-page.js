// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(async function() {
  TestRunner.addResult(`This tests that layout test can evaluate scripts in the inspected page.\n`);
  await TestRunner.evaluateInPagePromise(`
      function sum(a, b)
      {
          return a + b;
      }
  `);

  function callback(result) {
    TestRunner.addResult('2 + 2 = ' + result.description);
    TestRunner.completeTest();
  }
  TestRunner.evaluateInPage('sum(2, 2)', callback);
})();
