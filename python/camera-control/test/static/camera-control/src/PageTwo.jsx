import React from 'react';
import { useState, useEffect } from 'react';

import 'bootstrap/dist/css/bootstrap.min.css';
import Container from 'react-bootstrap/Container';
import Row from 'react-bootstrap/Row';
import Col from 'react-bootstrap/Col';
import Card from 'react-bootstrap/Card';
import { ListGroup } from 'react-bootstrap';
import Form from 'react-bootstrap/Form';
import { InputGroup } from 'react-bootstrap';
import Alert from 'react-bootstrap/Alert';

import { useAdapterEndpoint } from 'odin-react';
import { WithEndpoint } from 'odin-react';

function getMetadataByPath(metadata, path) {
    const keys = path.split('/').slice(1);
    let current = metadata;
    for (const key of keys) {
        if (current && typeof current === 'object') {
            current = current[key];
        } else {
            return null;
        }
    }
    return current;
}

function ConfigItem({ data, metadata, endpoint, parentKey = 'config', path = 'config' }) {
    if (typeof data === 'object' && data !== null) {
        return (
            <Card className="px-0">
                <Card.Header>{parentKey.split("_").map((word, i) => i === 0 ? word.charAt(0).toUpperCase() + word.slice(1) : word).join(" ")}</Card.Header>
                <ListGroup variant="flush" className="px-0">
                    {Object.entries(data).map(([key, value]) => {
                        const currentPath = `${path}/${key}`;
                        return (
                            <ListGroup.Item key={currentPath} className="px-2">
                                <ConfigItem data={value} metadata={metadata} endpoint={endpoint} parentKey={key} path={currentPath}/>
                            </ListGroup.Item>
                        );
                    })}
                </ListGroup>
            </Card>
        );
    } else {
        return (
            <Container>
                <Row>
                    <Col>
                        {/* <p>{(parentKey.split("_")[0].substring(0, 1).toUpperCase()).concat(parentKey.split("_")[0].slice(1).concat(" ",parentKey.split("_")[1]))}</p> */}
                        <p>{parentKey.split("_").map((word, i) => i === 0 ? word.charAt(0).toUpperCase() + word.slice(1) : word).join(" ")}</p>
                        {/* <Alert className="m-0 p-2">{parentKey}</Alert> */}
                    </Col>
                    <Col>
                        <ConfigInput writeable={getMetadataByPath(metadata, path)?.writeable} type={getMetadataByPath(metadata, path)?.type} value={getMetadataByPath(metadata, path)?.value} endpoint={endpoint} path={path}/>
                    </Col>
                </Row>
            </Container>
        );
    }
}



function ConfigInput({ writeable, type, value, endpoint, path }) {
    if (writeable === true) {
        if (type === "int" || type === "float") {
            return (
                <Container>
                    <EndpointInput
                        className="m-0 p-2"
                        endpoint={endpoint}
                        event_type="change"
                        type="number"
                        // step={1}
                        fullpath={`cameras/aravis/${path}`}/>
                </Container>
            );
        } if (type === "str") {
            return (
                <Container>
                    <EndpointInput
                        className="m-0 p-2"
                        endpoint={endpoint}
                        event_type="change"
                        type="string"
                        fullpath={`cameras/aravis/${path}`}/>
                </Container>
            );
        // Add option for boolean!
        } else {
            return (
                <Container>
                    <Alert className="m-0 p-2" variant={'danger'}>Unsupported type: {type}</Alert>
                </Container>
            );
        }
    } else {
        return (
            <Container>
                <p>{value}</p>
            </Container>
        )
    }
}


const EndpointInput = WithEndpoint(Form.Control)



function PageTwo(props) {

    // const endpoint = props
    const endpoint = useAdapterEndpoint("camera_control",  import.meta.env.VITE_ENDPOINT_URL, 1000);

    const configData = endpoint.data?.cameras?.aravis?.config;

    const configMetadata = endpoint.metadata?.cameras?.aravis?.config ? endpoint.metadata.cameras.aravis.config : {};

    return (

        <Container>
            <Row>
                <p></p>
                {configData ? <ConfigItem data={configData} metadata={configMetadata} endpoint={endpoint}/> : <p></p>}
            </Row>
            {/* <Row>
                <p></p>
                <pre>{JSON.stringify(metadata, null, 2)}</pre>
            </Row> */}
        </Container>

    )
}

export default PageTwo;