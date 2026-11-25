import { useRef, useEffect } from "react";
import { MediaPlayer } from "dashjs";
const API_BASE = import.meta.env.DEV ? `http://${window.location.hostname}:8080` : "";

export function CompetingPlayer() {
  const videoRef = useRef(null);
  const manifestUrl = `${API_BASE}/segments/manifest.mpd`;

  useEffect(() => {
    const player = MediaPlayer().create();
    player.initialize(videoRef.current, manifestUrl, false);

    return () => player.reset();
  }, []);

  return <video ref={videoRef} controls autoPlay></video>;
}
