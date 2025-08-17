<?php
session_start();

$errors = [];
$old = ['name' => '', 'email' => '', 'message' => ''];

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $name = trim($_POST['name'] ?? '');
    $email = trim($_POST['email'] ?? '');
    $message = trim($_POST['message'] ?? '');

    $old = ['name' => $name, 'email' => $email, 'message' => $message];

    if ($name === '') {
        $errors[] = 'Name is required.';
    }
    if ($email === '' || !filter_var($email, FILTER_VALIDATE_EMAIL)) {
        $errors[] = 'A valid email is required.';
    }
    if ($message === '') {
        $errors[] = 'Message is required.';
    }

    if (empty($errors)) {
        // store sanitized-ish data in session and redirect to view.php
        $_SESSION['form_data'] = $old;
        header('Location: view.php');
        exit;
    }
}
?>

<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Form</title>
  <link rel="stylesheet" href="../../../css/app.css">
</head>
<body>
  <h1>Submit form</h1>

  <?php if ($errors): ?>
    <ul style="color: red;">
      <?php foreach ($errors as $e): ?>
        <li><?php echo htmlspecialchars($e, ENT_QUOTES, 'UTF-8'); ?></li>
      <?php endforeach; ?>
    </ul>
  <?php endif; ?>

  <form method="post" action="">
    <label>
      Name:<br>
      <input type="text" name="name" value="<?php echo htmlspecialchars($old['name'], ENT_QUOTES, 'UTF-8'); ?>">
    </label>
    <br><br>
    <label>
      Email:<br>
      <input type="email" name="email" value="<?php echo htmlspecialchars($old['email'], ENT_QUOTES, 'UTF-8'); ?>">
    </label>
    <br><br>
    <label>
      Message:<br>
      <textarea name="message" rows="6" cols="40"><?php echo htmlspecialchars($old['message'], ENT_QUOTES, 'UTF-8'); ?></textarea>
    </label>
    <br><br>
    <button type="submit">Send</button>
  </form>
</body>
</html>