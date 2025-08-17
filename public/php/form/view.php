<?php
session_start();

if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['clear'])) {
    unset($_SESSION['form_data']);
    header('Location: index.php');
    exit;
}

$form = $_SESSION['form_data'] ?? null;
if (!$form) {
    header('Location: index.php');
    exit;
}
?>

<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>View submitted form</title>
</head>
<body>
  <h1>Form data</h1>
  <p><strong>Name:</strong> <?php echo htmlspecialchars($form['name'], ENT_QUOTES, 'UTF-8'); ?></p>
  <p><strong>Email:</strong> <?php echo htmlspecialchars($form['email'], ENT_QUOTES, 'UTF-8'); ?></p>
  <p><strong>Message:</strong><br><?php echo nl2br(htmlspecialchars($form['message'], ENT_QUOTES, 'UTF-8')); ?></p>

  <form method="post" action="">
    <button type="submit" name="clear">Clear and go back</button>
  </form>

  <p><a href="index.php">Edit</a></p>
</body>
</html>