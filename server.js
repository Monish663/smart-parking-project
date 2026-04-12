const cors = require("cors");
const express = require("express");
const app = express();
app.use(cors());
app.use(express.json());

let currentPIN = "1234"; // default PIN

// Get current PIN
app.get("/api/pin", (req, res) => {
  res.json({ pin: currentPIN });
});

// Book a slot (set new PIN)
app.post("/api/booking", (req, res) => {
  currentPIN = req.body.pin;
  res.json({ message: "PIN updated" });
});

app.post("/api/gate-event", (req, res) => {
  if(req.body.event === "closed") {
    currentPIN = String(Math.floor(1000 + Math.random() * 9000));
  }
  res.json({ ok: true });
});

// ESP32 posts slot sensor data here
app.post("/api/status", (req, res) => {
  res.json({ ok: true });
});

// Start server
const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log("Server running on port " + PORT));