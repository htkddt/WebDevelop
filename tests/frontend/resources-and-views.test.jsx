import React from 'react';
import { render, screen, waitFor } from '@testing-library/react';
import useApiResource from '../../frontend/src/hooks/useApiResource';
import DashboardView from '../../frontend/src/views/DashboardView';
import EmployeesView from '../../frontend/src/views/EmployeesView';

const ResourceProbe = ({ loadResource, fallbackData }) => {
  const resource = useApiResource(loadResource, fallbackData);
  return <output>{JSON.stringify(resource)}</output>;
};

describe('useApiResource', () => {
  it('exposes API data after a successful load', async () => {
    const loadResource = jest.fn().mockResolvedValue({ total: 3 });
    render(<ResourceProbe loadResource={loadResource} fallbackData={{ total: -1 }} />);

    await waitFor(() => expect(screen.getByText(/"loading":false/)).toHaveTextContent('"total":3'));
    expect(screen.getByText(/"connected":true/)).toBeInTheDocument();
  });

  it('keeps fallback data and marks the connection down after a failed load', async () => {
    const consoleError = jest.spyOn(console, 'error').mockImplementation(() => {});
    render(<ResourceProbe loadResource={jest.fn().mockRejectedValue(new Error('offline'))} fallbackData={{ total: -1 }} />);

    await waitFor(() => expect(screen.getByText(/"loading":false/)).toHaveTextContent('"total":-1'));
    expect(screen.getByText(/"connected":false/)).toBeInTheDocument();
    consoleError.mockRestore();
  });
});

describe('data views', () => {
  it('renders dashboard totals and disconnected status', () => {
    render(<DashboardView loading={false} connected={false} stats={{ departments: 2, total: 8, active: 7, inactive: 1 }} />);

    expect(screen.getByText('2')).toBeInTheDocument();
    expect(screen.getByText('8')).toBeInTheDocument();
    expect(screen.getByText('🔴')).toBeInTheDocument();
  });

  it('shows loading and empty states before rendering employees', () => {
    const { rerender } = render(<EmployeesView loading connected data={[]} />);
    expect(screen.getByText('Loading data from Backend ...')).toBeInTheDocument();

    rerender(<EmployeesView loading={false} connected data={[]} />);
    expect(screen.getByText('No staff')).toBeInTheDocument();
  });

  it('formats employee fields and supports both permission property names', () => {
    render(<EmployeesView loading={false} connected data={[{
      id: 'u1', username: 'Kiet', email: 'kiet@klose.dev', dept: 'Engineering',
      role: 'staff', permission: 'view', status: 'active',
    }]} />);

    expect(screen.getByText('Kiet')).toBeInTheDocument();
    expect(screen.getByText('Staff')).toBeInTheDocument();
    expect(screen.getByText('View')).toBeInTheDocument();
    expect(screen.getByText('Active')).toHaveClass('active');
  });
});
