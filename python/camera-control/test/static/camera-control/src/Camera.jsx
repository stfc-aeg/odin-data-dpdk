import React from 'react';
import 'bootstrap/dist/css/bootstrap.min.css';

import Container from 'react-bootstrap/Container';
import Row from 'react-bootstrap/Row';
import Col from 'react-bootstrap/Col';
import Button from 'react-bootstrap/Button';
import { WithEndpoint } from 'odin-react';
import { OdinLiveView } from 'odin-react';

import TreeDisplay from './TreeDisplay';

const EndPointButton = WithEndpoint(Button);

function CameraPreview(props) {
    const {name, endpoint, liveEndpoint, data} = props;

    const state = data.status?.state ? data.status.state : "disconnected";
    const status = ['disconnected', 'connected', 'capturing'];

    return (
        <Container className="m-0">
                <Row>
                    <p></p>
                    <OdinLiveView
                        endpoint={liveEndpoint}
                        img_path={`liveview/${name}/image_data`}
                        addrs={{
                            colormap_selected_addr: `liveview/${name}/image_metadata/colour`,
                            colormap_options_addr: `liveview/${name}/image_metadata/colour_options`,
                            clip_range_addr: `liveview/${name}/image_metadata/clip_range_value`,
                            min_max_addr: `liveview/${name}/image_metadata/data_min_max`
                            // frame_num_addr: "aravis/image_metadata/frame_num"
                        }}
                    />
                </Row>
                <Row className='g-5'>
                    <Col xs={6}>
                            <Row>
                                <TreeDisplay
                                    name={name}
                                    endpoint={endpoint}
                                    metadata={endpoint.metadata?.cameras?.[name]?.status ?? {}}
                                    data={data.status}
                                    path={'cameras/aravis/status'}
                                    paths={['cameras/aravis/status/state']}
                                    />
                            </Row>
                            <Row>
                                <p></p>
                                <EndPointButton // Move between statuses 1 and 0
                                endpoint={endpoint}
                                value={state!==status[0] ? "disconnect" : "connect"}
                                fullpath={`cameras/${name}/command`}
                                event_type="click"
                                disabled={![status[1], status[0]].includes(state)}
                                variant={state!==status[0] ? "warning" : "success"}>
                                    {state==status[0] ? 'Connect' : 'Disconnect'}
                                </EndPointButton>
                            </Row>
                            <Row>
                                <p></p>
                                <EndPointButton // Move between statuses 3 and 1
                                endpoint={endpoint}
                                value={state===status[2] ? "stop" : "start"}
                                fullpath={`cameras/${name}/command`}
                                event_type="click"
                                disabled={![status[2], status[1]].includes(state)}
                                variant={state===status[2] ? "warning" : "success"}>
                                {state===status[2] ? 'Stop Capturing' : 'Capture'}
                                </EndPointButton>
                            </Row>
                    </Col>
                    <Col xs={6}>
                        <TreeDisplay
                            name={name}
                            endpoint={endpoint}
                            metadata={endpoint.metadata?.cameras?.[name]?.status ?? {}}
                            data={data.status}
                            path={'cameras/aravis/status'}
                            paths={['cameras/aravis/status/frame_number', 'cameras/aravis/status/frame_errors', 'cameras/aravis/status/frame_timeouts', 'cameras/aravis/status/id', 'cameras/aravis/status/model']}
                            />
                    </Col>
                </Row>
        </Container>
    )
};

function Camera(props) {

    const {endpoint, liveEndpoint, name, data} = props;

    return (

        <Container>
            <Row>
                <Col xs={7}>
                    <p></p>
                    <CameraPreview
                        name={name}
                        endpoint={endpoint}
                        liveEndpoint={liveEndpoint}
                        data={data} />
                </Col>
                <Col xs={5}>
                    <p></p>
                    <TreeDisplay
                    name={name}
                    endpoint={endpoint}
                    metadata={endpoint.metadata?.cameras?.[name]?.config ?? {}}
                    data={data.config}
                    path={'cameras/aravis/config'}/>
                </Col>
            </Row>
        </Container>

    )
}

export default Camera;