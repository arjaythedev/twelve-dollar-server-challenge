<?php

declare(strict_types=1);

// auto_prepend_file: samples the PHP thread's CPU clock every 0.1 ms, from before index.php runs until the script
// ends. In classic mode that's one request, Symfony's boot included; in worker mode, a worker's whole life (restarted
// every 500 requests by public/index.php). Collapsed stacks are appended to /tmp/profile/stacks.txt.
// EXCIMER_EVENT=real samples wall-clock time instead: waits included (SQLite's locks, opening the database).
if ('cli' === \PHP_SAPI || !extension_loaded('excimer')) {
    return;
}

$GLOBALS['__excimer'] = $profiler = new ExcimerProfiler();
$profiler->setEventType('real' === getenv('EXCIMER_EVENT') ? EXCIMER_REAL : EXCIMER_CPU);
$profiler->setPeriod(0.0001);
$profiler->setMaxDepth(200);
$write = static function (ExcimerLog $log): void {
    file_put_contents('/tmp/profile/stacks.txt', $log->formatCollapsed(), \FILE_APPEND | \LOCK_EX);
};
$profiler->setFlushCallback($write, 2000);
// stop() hands what's left to the flush callback: flushing again would write those samples twice
register_shutdown_function(static function () use ($profiler): void {
    $profiler->stop();
});
$profiler->start();
