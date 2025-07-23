import React from 'react';
import { useState, useEffect } from 'react';

import 'bootstrap/dist/css/bootstrap.min.css';
import Container from 'react-bootstrap/Container';
import InputGroup from 'react-bootstrap/InputGroup';
import Form from 'react-bootstrap/Form';
import Row from 'react-bootstrap/Row';
import Col from 'react-bootstrap/Col';
import ToggleButtonGroup from 'react-bootstrap/ToggleButtonGroup';
import ToggleButton from 'react-bootstrap/ToggleButton';
import Button from 'react-bootstrap/Button';
import Card from 'react-bootstrap/Card';

import { useAdapterEndpoint } from 'odin-react';
import { OdinEventLog } from 'odin-react';
import { WithEndpoint } from 'odin-react';
import { OdinLiveView } from 'odin-react';

// import ClickableImage from './ClickableImage';

const EndPointFormControl = WithEndpoint(Form.Control);
const EndpointToggleButton = WithEndpoint(ToggleButton);

const EndPointButton = WithEndpoint(Button);



function PageOne(props) {

    // const endpoint = props

    const endpoint = useAdapterEndpoint("camera_control",  import.meta.env.VITE_ENDPOINT_URL, 1000);
    const liveEndpoint = useAdapterEndpoint("live_data", import.meta.env.VITE_ENDPOINT_URL);
    const state = endpoint.data?.cameras?.aravis?.status?.state ? endpoint.data.cameras.aravis.status.state : "disconnected";
    const status = ['disconnected', 'connected', 'capturing'];


    return (

        <Container>
            <Row>
                <Col xs={3}>
                <Form>
                    <Row>
                        <p></p>
                        <Card className="text-center">
                            <Card.Body>{"state: " + state || "not found"}</Card.Body>
                        </Card>
                        <p></p>
                        <EndPointButton // Move between statuses 1 and 0
                        endpoint={endpoint}
                        value={state!==status[0] ? "disconnect" : "connect"}
                        fullpath="cameras/aravis/command"
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
                        fullpath="cameras/aravis/command"
                        event_type="click"
                        disabled={![status[2], status[1]].includes(state)}
                        variant={state===status[2] ? "warning" : "success"}>
                        {state===status[2] ? 'Stop Capturing' : 'Capture'}
                        </EndPointButton>
                    </Row>
                </Form>
                </Col>
                <Col xs={9}>
                    <p></p>
                    <OdinLiveView
                        endpoint={liveEndpoint}
                        img_path="liveview/aravis/image_data"
                        addrs={{
                            colormap_selected_addr: "liveview/aravis/image_metadata/colour",
                            colormap_options_addr: "liveview/aravis/image_metadata/colour_options",
                            clip_range_addr: "liveview/aravis/image_metadata/clip_range_value",
                            min_max_addr: "liveview/aravis/image_metadata/data_min_max"
                            // frame_num_addr: "aravis/image_metadata/frame_num"
                        }}
                    />
                </Col>
            </Row>
        </Container>

    )
}

export default PageOne;