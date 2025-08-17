console.log('app.js loaded');
// dummy dependency content
function greet(){
  alert('hello from app.js');
}

document.addEventListener('DOMContentLoaded', function(){
  var btn = document.getElementById('greetBtn');
  if (btn) btn.addEventListener('click', greet);
});
