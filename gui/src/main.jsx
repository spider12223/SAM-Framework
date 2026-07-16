import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App.jsx';
import './index.css';
import './cursor.css';   // Barony gauntlet cursor — after index.css so its :root vars win
import './cursorPress.js'; // swaps to the pressed gauntlet on click, like the game

ReactDOM.createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
);
