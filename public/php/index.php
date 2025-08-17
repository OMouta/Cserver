<?php

// Simple PHP info page with key installation details and optional full phpinfo()

function bytes_to_readable($bytes) {
    if ($bytes === false) return 'N/A';
    $units = ['B','K','M','G','T'];
    $i = 0;
    while ($bytes >= 1024 && $i < count($units)-1) {
        $bytes /= 1024;
        $i++;
    }
    return round($bytes, 2) . $units[$i];
}

function parse_ini_size($val) {
    if ($val === false) return false;
    $val = trim($val);
    $last = strtolower($val[strlen($val)-1]);
    $num = (int)$val;
    switch($last) {
        case 'g': $num *= 1024;
        case 'm': $num *= 1024;
        case 'k': $num *= 1024;
    }
    return $num;
}

$showFull = isset($_GET['full']) && $_GET['full'] === '1';

// Collect basic info
$info = [
    'PHP Version' => phpversion(),
    'SAPI' => php_sapi_name(),
    'OS' => PHP_OS . ' (' . PHP_OS_FAMILY . ')',
    'Architecture' => PHP_INT_SIZE * 8 . '-bit',
    'PHP Binary' => PHP_BINARY ?? 'N/A',
    'Loaded php.ini' => php_ini_loaded_file() ?: 'none',
    'Scanned INI files' => php_ini_scanned_files() ?: 'none',
    'Configuration (memory_limit)' => ini_get('memory_limit'),
    'Configuration (post_max_size)' => ini_get('post_max_size'),
    'Configuration (upload_max_filesize)' => ini_get('upload_max_filesize'),
    'Max execution time' => ini_get('max_execution_time') . 's',
    'Disabled functions' => ini_get('disable_functions') ?: 'none',
    'Default timezone' => ini_get('date.timezone') ?: date_default_timezone_get(),
    'Timezone (now)' => (new DateTime())->format(DateTime::RFC1123),
    'Loaded extensions (count)' => count(get_loaded_extensions()),
    'PDO drivers' => implode(', ', PDO::getAvailableDrivers()),
];

// Extensions list
$exts = get_loaded_extensions();
sort($exts, SORT_NATURAL | SORT_FLAG_CASE);
$extList = $exts;

// Server variables (selected)
$server = [
    'Server Software' => $_SERVER['SERVER_SOFTWARE'] ?? php_sapi_name(),
    'Document Root' => $_SERVER['DOCUMENT_ROOT'] ?? 'N/A',
    'Server Name' => $_SERVER['SERVER_NAME'] ?? 'N/A',
    'Request URI' => $_SERVER['REQUEST_URI'] ?? 'N/A',
];

?>

<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>PHP Installation Details</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
    body { font-family: system-ui, -apple-system, "Segoe UI", Roboto, Arial; margin: 24px; color: #111; background: #f7f7fb; }
    h1 { margin-bottom: 8px; }
    .wrap { max-width: 1000px; margin: 0 auto; }
    table { width: 100%; border-collapse: collapse; background: #fff; box-shadow: 0 1px 2px rgba(0,0,0,.05); }
    th, td { text-align: left; padding: 10px 12px; border-bottom: 1px solid #eee; font-size: 14px; }
    th { width: 280px; background: #fafafa; color: #333; font-weight: 600; }
    pre { margin: 6px 0; padding: 8px; background:#f3f4f6; border-radius:4px; overflow:auto; }
    .actions { margin: 12px 0; }
    a.button { display:inline-block; padding:8px 12px; background:#0b5fff; color:#fff; text-decoration:none; border-radius:6px; font-size:14px; }
    .small { font-size: 13px; color:#666; }
    ul.ext { columns: 3; -webkit-columns:3; -moz-columns:3; list-style: none; padding-left: 0; margin:0; }
    ul.ext li { padding: 3px 0; }
</style>
</head>
<body>
<div class="wrap">
    <h1>PHP Installation Details</h1>
    <p class="small">A concise view of the current PHP runtime. <a href="?full=<?= $showFull ? '0' : '1' ?>" class="button"><?= $showFull ? 'Hide phpinfo()' : 'Show full phpinfo()' ?></a></p>

    <table>
        <tbody>
        <?php foreach ($info as $k => $v): ?>
            <tr>
                <th><?= htmlspecialchars($k) ?></th>
                <td>
                    <?php if (is_array($v)): ?>
                        <pre><?= htmlspecialchars(print_r($v, true)) ?></pre>
                    <?php else: ?>
                        <?= htmlspecialchars((string)$v) ?>
                    <?php endif ?>
                </td>
            </tr>
        <?php endforeach; ?>
            <tr>
                <th>Loaded extensions (list)</th>
                <td>
                    <ul class="ext">
                        <?php foreach ($extList as $e): ?>
                            <li><?= htmlspecialchars($e) ?></li>
                        <?php endforeach; ?>
                    </ul>
                </td>
            </tr>
        <?php foreach ($server as $k => $v): ?>
            <tr>
                <th><?= htmlspecialchars($k) ?></th>
                <td><?= htmlspecialchars($v) ?></td>
            </tr>
        <?php endforeach; ?>
        </tbody>
    </table>

    <?php if ($showFull): ?>
        <h2 style="margin-top:18px;">Full phpinfo()</h2>
        <div style="background:#fff;padding:12px;border-radius:6px;box-shadow:0 1px 2px rgba(0,0,0,.04);">
            <?php phpinfo(); ?>
        </div>
    <?php endif; ?>
</div>
</body>
</html>