// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.components.background_task_scheduler;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.mockito.Mockito.doNothing;
import static org.mockito.Mockito.doReturn;
import static org.mockito.Mockito.eq;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;

import android.os.Build;

import com.google.android.gms.common.ConnectionResult;
import com.google.android.gms.common.GoogleApiAvailability;
import com.google.android.gms.gcm.GcmNetworkManager;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.shadow.api.Shadow;
import org.robolectric.shadows.gms.Shadows;
import org.robolectric.shadows.gms.common.ShadowGoogleApiAvailability;
import org.robolectric.util.ReflectionHelpers;

import org.chromium.base.ContextUtils;
import org.chromium.base.test.util.Feature;
import org.chromium.testing.local.LocalRobolectricTestRunner;

import java.util.concurrent.TimeUnit;

/** Unit tests for {@link BackgroundTaskScheduler}. */
@RunWith(LocalRobolectricTestRunner.class)
@Config(manifest = Config.NONE,
        shadows = {ShadowGcmNetworkManager.class, ShadowGoogleApiAvailability.class})
public class BackgroundTaskSchedulerImplTest {
    private static final TaskInfo TASK =
            TaskInfo.createOneOffTask(
                            TaskIds.TEST, TestBackgroundTask.class, TimeUnit.DAYS.toMillis(1))
                    .build();

    @Mock
    private BackgroundTaskSchedulerDelegate mDelegate;
    @Mock
    private BackgroundTaskSchedulerUma mBackgroundTaskSchedulerUma;
    private ShadowGcmNetworkManager mGcmNetworkManager;

    @Before
    public void setUp() {
        MockitoAnnotations.initMocks(this);
        ContextUtils.initApplicationContextForTests(RuntimeEnvironment.application);
        BackgroundTaskSchedulerFactory.setSchedulerForTesting(
                new BackgroundTaskSchedulerImpl(mDelegate));
        BackgroundTaskSchedulerUma.setInstanceForTesting(mBackgroundTaskSchedulerUma);
        TestBackgroundTask.reset();

        // Initialize Google Play Services and GCM Network Manager for upgrade testing.
        Shadows.shadowOf(GoogleApiAvailability.getInstance())
                .setIsGooglePlayServicesAvailable(ConnectionResult.SUCCESS);
        mGcmNetworkManager = (ShadowGcmNetworkManager) Shadow.extract(
                GcmNetworkManager.getInstance(ContextUtils.getApplicationContext()));
    }

    @Test
    @Feature({"BackgroundTaskScheduler"})
    public void testScheduleTaskSuccessful() {
        doReturn(true).when(mDelegate).schedule(eq(RuntimeEnvironment.application), eq(TASK));
        BackgroundTaskSchedulerFactory.getScheduler().schedule(
                RuntimeEnvironment.application, TASK);
        assertTrue(BackgroundTaskSchedulerPrefs.getScheduledTasks().contains(
                TASK.getBackgroundTaskClass().getName()));
        verify(mDelegate, times(1)).schedule(eq(RuntimeEnvironment.application), eq(TASK));
        verify(mBackgroundTaskSchedulerUma, times(1))
                .reportTaskScheduled(eq(TaskIds.TEST), eq(true));
    }

    @Test
    @Feature({"BackgroundTaskScheduler"})
    public void testScheduleTaskFailed() {
        doReturn(false).when(mDelegate).schedule(eq(RuntimeEnvironment.application), eq(TASK));
        BackgroundTaskSchedulerFactory.getScheduler().schedule(
                RuntimeEnvironment.application, TASK);
        assertFalse(BackgroundTaskSchedulerPrefs.getScheduledTasks().contains(
                TASK.getBackgroundTaskClass().getName()));
        verify(mDelegate, times(1)).schedule(eq(RuntimeEnvironment.application), eq(TASK));
    }

    @Test
    @Feature({"BackgroundTaskScheduler"})
    public void testCancel() {
        BackgroundTaskSchedulerPrefs.addScheduledTask(TASK);

        doNothing().when(mDelegate).cancel(eq(RuntimeEnvironment.application), eq(TaskIds.TEST));
        BackgroundTaskSchedulerFactory.getScheduler().cancel(
                RuntimeEnvironment.application, TaskIds.TEST);
        assertFalse(BackgroundTaskSchedulerPrefs.getScheduledTasks().contains(
                TASK.getBackgroundTaskClass().getName()));
        verify(mDelegate, times(1)).cancel(eq(RuntimeEnvironment.application), eq(TaskIds.TEST));
    }

    @Test
    @Feature({"BackgroundTaskScheduler"})
    public void testRescheduleTasks() {
        BackgroundTaskSchedulerPrefs.addScheduledTask(TASK);

        assertEquals(0, TestBackgroundTask.getRescheduleCalls());
        assertFalse(BackgroundTaskSchedulerPrefs.getScheduledTasks().isEmpty());
        BackgroundTaskSchedulerFactory.getScheduler().reschedule(RuntimeEnvironment.application);

        assertEquals(1, TestBackgroundTask.getRescheduleCalls());
        assertTrue(BackgroundTaskSchedulerPrefs.getScheduledTasks().isEmpty());
    }

    @Test
    @Feature({"BackgroundTaskScheduler"})
    public void testCheckForOSUpgrade_PreMToMPlus() {
        BackgroundTaskSchedulerPrefs.setLastSdkVersion(Build.VERSION_CODES.LOLLIPOP);
        BackgroundTaskSchedulerPrefs.addScheduledTask(TASK);
        ReflectionHelpers.setStaticField(Build.VERSION.class, "SDK_INT", Build.VERSION_CODES.M);

        BackgroundTaskSchedulerFactory.getScheduler().checkForOSUpgrade(
                RuntimeEnvironment.application);

        assertEquals(Build.VERSION_CODES.M, BackgroundTaskSchedulerPrefs.getLastSdkVersion());
        assertTrue(mGcmNetworkManager.getCanceledTaskTags().contains(
                Integer.toString(TASK.getTaskId())));
        assertEquals(1, TestBackgroundTask.getRescheduleCalls());
    }

    /** This scenario tests upgrade from pre-M to pre-M OS, which requires no rescheduling. */
    @Test
    @Feature({"BackgroundTaskScheduler"})
    public void testCheckForOSUpgrade_PreMToPreM() {
        BackgroundTaskSchedulerPrefs.setLastSdkVersion(Build.VERSION_CODES.KITKAT);
        BackgroundTaskSchedulerPrefs.addScheduledTask(TASK);
        ReflectionHelpers.setStaticField(
                Build.VERSION.class, "SDK_INT", Build.VERSION_CODES.LOLLIPOP);

        BackgroundTaskSchedulerFactory.getScheduler().checkForOSUpgrade(
                RuntimeEnvironment.application);

        assertEquals(
                Build.VERSION_CODES.LOLLIPOP, BackgroundTaskSchedulerPrefs.getLastSdkVersion());
        assertEquals(0, TestBackgroundTask.getRescheduleCalls());
    }

    /** This scenario tests upgrade from M+ to M+ OS, which requires no rescheduling. */
    @Test
    @Feature({"BackgroundTaskScheduler"})
    public void testCheckForOSUpgrade_MPlusToMPlus() {
        BackgroundTaskSchedulerPrefs.setLastSdkVersion(Build.VERSION_CODES.M);
        BackgroundTaskSchedulerPrefs.addScheduledTask(TASK);
        ReflectionHelpers.setStaticField(Build.VERSION.class, "SDK_INT", Build.VERSION_CODES.N);

        BackgroundTaskSchedulerFactory.getScheduler().checkForOSUpgrade(
                RuntimeEnvironment.application);

        assertEquals(Build.VERSION_CODES.N, BackgroundTaskSchedulerPrefs.getLastSdkVersion());
        assertEquals(0, TestBackgroundTask.getRescheduleCalls());
    }
}
