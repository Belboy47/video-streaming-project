import { useState, useRef } from "react";

import "./App.css";

function App() {
  const [source, setSource] = useState(720);
  const videoRef = useRef(null);
  const savedTime = useRef(0);
  const wasPaused = useRef(0);

  function setPlaybackTime() {
    savedTime.current = videoRef.current.currentTime;
    wasPaused.current = videoRef.current.paused;
  }

  return (
    <>
      <div>
        <h1>Video Player</h1> <h2>Current Quality: {source}</h2>
        <video
          ref={videoRef}
          src={`http://localhost:8080/video/${source}`}
          controls
          onLoadedMetadata={() => {
            videoRef.current.currentTime = savedTime.current;
            if (!wasPaused.current) {
              videoRef.current.play();
            }
          }}
        ></video>
        <button
          onClick={() => {
            setPlaybackTime();
            setSource(480);
          }}
        >
          480p
        </button>
        <button
          onClick={() => {
            setPlaybackTime();
            setSource(720);
          }}
        >
          720p
        </button>
        <button
          onClick={() => {
            setPlaybackTime();
            setSource(1080);
          }}
        >
          1080p
        </button>
      </div>
    </>
  );
}
export default App;
