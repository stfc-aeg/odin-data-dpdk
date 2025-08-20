import React from 'react';
import 'bootstrap/dist/css/bootstrap.min.css';
import { OdinApp } from 'odin-react';
import { useAdapterEndpoint } from 'odin-react';

import Camera from './Camera';

function App() {

  const endpoint = useAdapterEndpoint("camera_control",  import.meta.env.VITE_ENDPOINT_URL, 1000);
  const liveEndpoint = useAdapterEndpoint("live_data", import.meta.env.VITE_ENDPOINT_URL);

  const CameraTabs = endpoint.data?.cameras ? Object.entries(endpoint.data?.cameras).map(
    ([key, value]) => (
        <Camera endpoint={endpoint} liveEndpoint={liveEndpoint} name={key} data={value}/>
    )
  ) : <></>

  const cameraNames = endpoint.data?.cameras ? Object.entries(endpoint.data.cameras).map(
    ([key, value]) => (
        key.split(" ").map((word, i) => i === 0 ? word.charAt(0).toUpperCase() + word.slice(1) : word).join(" ") + (value?.config?.camera_connection ? (" (" + value.config.camera_connection + ")") : "")
    )
  ) : [];

  return (

    <OdinApp title="Camera Adapter" navLinks={cameraNames}>

      {CameraTabs}

    </OdinApp>
  )
}

export default App
