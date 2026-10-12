import PageContent from '../components/PageContent';
import useApiResource from '../hooks/useApiResource';
import { employeesApi } from '../services/api';
import EmployeesView from '../views/EmployeesView';

const FALLBACK_EMPLOYEES = [{
  id: 'offline',
  username: 'Null',
  email: 'Null',
  dept: 'Null',
  role: 'Null',
  permissions: 'Null',
  status: 'inactive',
}];

const EmployeesPage = () => {
  const { data: employees, loading, connected } = useApiResource(
    employeesApi.getAll,
    FALLBACK_EMPLOYEES,
  );

  return (
    <PageContent>
      <EmployeesView data={employees} loading={loading} connected={connected} />
    </PageContent>
  );
};

export default EmployeesPage;
